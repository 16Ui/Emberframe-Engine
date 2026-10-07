"""为本地试用生成静态、512px 贴图 GLB；不启动引擎，不执行测试。

原资源不改动。VRM 使用默认节点姿势和默认 morph 权重烘焙蒙皮；
仅保留通用 glTF 材质回退，不模拟 MToon、表情控制或弹簧骨骼。
依赖：numpy、Pillow。来源与许可见 assets/model_library/sources。
"""
from __future__ import annotations

import base64
import copy
import io
import json
import struct
from pathlib import Path
from urllib.parse import unquote

import numpy as np
from PIL import Image

ROOT = Path(__file__).resolve().parents[1] / "assets" / "model_library"
MAX_TEXTURE = 512
KEPT_EXTENSIONS = {
    "KHR_mesh_quantization", "KHR_texture_transform",
    "KHR_materials_clearcoat", "KHR_materials_emissive_strength",
    "KHR_lights_punctual",
}
DTYPES = {5120: "i1", 5121: "u1", 5122: "<i2", 5123: "<u2", 5125: "<u4", 5126: "<f4"}
WIDTHS = {"SCALAR": 1, "VEC2": 2, "VEC3": 3, "VEC4": 4, "MAT4": 16}


def read_document(path):
    raw = path.read_bytes()
    binary = b""
    if path.suffix.lower() in {".glb", ".vrm"}:
        magic, version, length = struct.unpack_from("<III", raw)
        if magic != 0x46546C67 or version != 2 or length != len(raw):
            raise ValueError("Not a complete glTF 2 GLB container")
        offset, doc = 12, None
        while offset < len(raw):
            size, kind = struct.unpack_from("<II", raw, offset)
            payload = raw[offset + 8:offset + 8 + size]
            if kind == 0x4E4F534A:
                doc = json.loads(payload)
            elif kind == 0x004E4942:
                binary = payload
            offset += 8 + size
        if doc is None:
            raise ValueError("Missing JSON chunk")
    else:
        doc = json.loads(raw)

    def dependency(uri):
        if uri.startswith("data:"):
            header, data = uri.split(",", 1)
            if ";base64" not in header:
                raise ValueError("Only base64 data URIs are supported")
            return base64.b64decode(data)
        target = (path.parent / unquote(uri)).resolve()
        if not target.is_relative_to(path.parent.resolve()):
            raise ValueError("Dependency must remain inside the original asset directory")
        return target.read_bytes()

    buffers = [dependency(b["uri"]) if "uri" in b else binary for b in doc.get("buffers", [])]
    return doc, buffers, dependency


class Asset:
    def __init__(self, path):
        self.path = path
        self.doc, buffers, self.dependency = read_document(path)
        self.views = [buffers[v["buffer"]][v.get("byteOffset", 0):v.get("byteOffset", 0) + v["byteLength"]]
                      for v in self.doc.get("bufferViews", [])]

    def accessor(self, index):
        item = self.doc["accessors"][index]
        dtype, width, count = np.dtype(DTYPES[item["componentType"]]), WIDTHS[item["type"]], item["count"]
        result = np.zeros((count, width), dtype=dtype)
        if "bufferView" in item:
            view = item["bufferView"]
            stride = self.doc["bufferViews"][view].get("byteStride", width * dtype.itemsize)
            result = np.ndarray((count, width), dtype=dtype, buffer=self.views[view],
                                offset=item.get("byteOffset", 0), strides=(stride, dtype.itemsize)).copy()
        if "sparse" in item:
            sparse = item["sparse"]
            idx, val = sparse["indices"], sparse["values"]
            indices = np.frombuffer(self.views[idx["bufferView"]], dtype=DTYPES[idx["componentType"]],
                                    count=sparse["count"], offset=idx.get("byteOffset", 0))
            values = np.frombuffer(self.views[val["bufferView"]], dtype=dtype,
                                   count=sparse["count"] * width, offset=val.get("byteOffset", 0)).reshape(-1, width)
            result[indices] = values
        if item.get("normalized") and dtype.kind in "iu":
            result = result.astype(np.float64) / np.iinfo(dtype).max
            if dtype.kind == "i":
                result = np.maximum(result, -1)
        return result

    def append_view(self, data):
        index = len(self.views)
        self.views.append(data)
        self.doc.setdefault("bufferViews", []).append({"buffer": 0, "byteLength": len(data)})
        return index

    def append_accessor(self, values, kind):
        values = np.asarray(values, dtype="<f4")
        item = {"bufferView": self.append_view(values.tobytes()), "componentType": 5126,
                "count": len(values), "type": kind}
        if kind == "VEC3":
            item.update(min=values.min(axis=0).tolist(), max=values.max(axis=0).tolist())
        self.doc.setdefault("accessors", []).append(item)
        return len(self.doc["accessors"]) - 1

    def world_matrices(self):
        nodes = self.doc.get("nodes", [])
        parents = {child: i for i, node in enumerate(nodes) for child in node.get("children", [])}
        cache, visiting = {}, set()

        def world(index):
            if index in cache:
                return cache[index]
            if index in visiting:
                raise ValueError("Cyclic node hierarchy")
            visiting.add(index)
            node = nodes[index]
            if "matrix" in node:
                local = np.array(node["matrix"], dtype=float).reshape(4, 4).T
            else:
                x, y, z, w = node.get("rotation", [0, 0, 0, 1])
                local = np.eye(4)
                local[:3, :3] = np.array([
                    [1-2*(y*y+z*z), 2*(x*y-z*w), 2*(x*z+y*w)],
                    [2*(x*y+z*w), 1-2*(x*x+z*z), 2*(y*z-x*w)],
                    [2*(x*z-y*w), 2*(y*z+x*w), 1-2*(x*x+y*y)]]) @ np.diag(node.get("scale", [1, 1, 1]))
                local[:3, 3] = node.get("translation", [0, 0, 0])
            cache[index] = world(parents[index]) @ local if index in parents else local
            visiting.remove(index)
            return cache[index]

        return [world(i) for i in range(len(nodes))]

    @staticmethod
    def normalize(v):
        return v / np.maximum(np.linalg.norm(v, axis=-1, keepdims=True), 1e-12)

    def bake_static(self):
        doc, meshes = self.doc, []
        worlds = self.world_matrices()
        original_meshes = doc.get("meshes", [])
        for node_index, node in enumerate(doc.get("nodes", [])):
            if "mesh" not in node:
                continue
            mesh = copy.deepcopy(original_meshes[node["mesh"]])
            weights = node.get("weights", mesh.get("weights", []))
            for primitive in mesh["primitives"]:
                attrs = primitive["attributes"]
                directions = {name: self.accessor(attrs[name]).astype(float)
                              for name in ("POSITION", "NORMAL", "TANGENT") if name in attrs}
                changed = False
                for target_index, target in enumerate(primitive.get("targets", [])):
                    weight = weights[target_index] if target_index < len(weights) else 0
                    if not weight:
                        continue
                    for name in directions.keys() & target.keys():
                        directions[name][:, :3] += weight * self.accessor(target[name])
                        changed = True
                if "skin" in node:
                    skin = doc["skins"][node["skin"]]
                    joints = skin["joints"]
                    inverse_bind = (self.accessor(skin["inverseBindMatrices"]).reshape(-1, 4, 4).transpose(0, 2, 1)
                                    if "inverseBindMatrices" in skin else np.repeat(np.eye(4)[None], len(joints), axis=0))
                    # 保留 Mesh Node 变换：骨骼先转换到该节点的局部空间，烘焙后由节点继续转到世界空间。
                    joint_matrices = np.array([np.linalg.inv(worlds[node_index]) @ worlds[joint] @ inverse_bind[k]
                                               for k, joint in enumerate(joints)])
                    count = len(directions["POSITION"])
                    blend, total = np.zeros((count, 4, 4)), np.zeros(count)
                    sets = [key for key in attrs if key.startswith("JOINTS_")]
                    if not sets:
                        raise ValueError("Skinned primitive has no joint attributes")
                    for key in sets:
                        ji = self.accessor(attrs[key]).astype(np.int64)
                        wt = self.accessor(attrs[key.replace("JOINTS_", "WEIGHTS_")]).astype(float)
                        blend += np.sum(joint_matrices[ji] * wt[:, :, None, None], axis=1)
                        total += wt.sum(axis=1)
                    blend /= np.maximum(total[:, None, None], 1e-12)
                    blend[total < 1e-12] = np.eye(4)
                    position = np.c_[directions["POSITION"], np.ones(count)]
                    directions["POSITION"] = np.einsum("nij,nj->ni", blend, position)[:, :3]
                    linear = blend[:, :3, :3]
                    if "NORMAL" in directions:
                        normals = np.einsum("nij,nj->ni", np.linalg.pinv(linear).transpose(0, 2, 1), directions["NORMAL"])
                        directions["NORMAL"] = self.normalize(normals)
                    if "TANGENT" in directions:
                        tangent = directions["TANGENT"]
                        tangent[:, :3] = self.normalize(np.einsum("nij,nj->ni", linear, tangent[:, :3]))
                        tangent[:, 3] *= np.where(np.linalg.det(linear) < 0, -1, 1)
                    changed = True
                if changed:
                    for name, values in directions.items():
                        attrs[name] = self.append_accessor(values, "VEC4" if name == "TANGENT" else "VEC3")
                for key in list(attrs):
                    if key.startswith(("JOINTS_", "WEIGHTS_")):
                        del attrs[key]
                primitive.pop("targets", None)
            mesh.pop("weights", None)
            node["mesh"] = len(meshes)
            node.pop("skin", None)
            node.pop("weights", None)
            meshes.append(mesh)
        doc["meshes"] = meshes
        doc.pop("skins", None)
        doc.pop("animations", None)

    def remove_special_extensions(self, value):
        if isinstance(value, dict):
            if "extensions" in value:
                value["extensions"] = {k: v for k, v in value["extensions"].items() if k in KEPT_EXTENSIONS}
                if not value["extensions"]:
                    del value["extensions"]
            for key, nested in list(value.items()):
                if key != "extras":
                    self.remove_special_extensions(nested)
        elif isinstance(value, list):
            for item in value:
                self.remove_special_extensions(item)

    def material_textures(self):
        """只处理还被保留的普通 glTF 材质引用，不携带 VRM 专属遮罩、缩略图。"""
        infos = []

        def collect(value):
            if isinstance(value, dict):
                for key, nested in value.items():
                    if key.endswith("Texture") and isinstance(nested, dict) and "index" in nested:
                        role = "normal" if key in {"normalTexture", "clearcoatNormalTexture"} else (
                            "srgb" if key in {"baseColorTexture", "emissiveTexture"} else "data")
                        infos.append((nested, role))
                    else:
                        collect(nested)
            elif isinstance(value, list):
                for item in value:
                    collect(item)

        collect(self.doc.get("materials", []))
        old_textures, old_images = self.doc.get("textures", []), self.doc.get("images", [])
        selected = sorted({info["index"] for info, _ in infos})
        remap = {old: new for new, old in enumerate(selected)}
        textures = [copy.deepcopy(old_textures[i]) for i in selected]
        roles = {}
        for info, role in infos:
            source = old_textures[info["index"]]["source"]
            roles.setdefault(source, set()).add(role)
            info["index"] = remap[info["index"]]
        images = sorted(roles)
        image_map = {old: new for new, old in enumerate(images)}
        for texture in textures:
            texture["source"] = image_map[texture["source"]]
        self.doc["textures"] = textures
        self.doc["images"] = [copy.deepcopy(old_images[i]) for i in images]
        return [roles[i] for i in images]

    def adapt_single_uv_materials(self):
        """当前引擎每个顶点只有一套 UV；显式准备近似材质，不伪装成完整扩展支持。"""
        changes, signatures = [], {}
        for material_index, material in enumerate(self.doc.get("materials", [])):
            coat = material.get("extensions", {}).get("KHR_materials_clearcoat", {})
            for key in ("clearcoatTexture", "clearcoatRoughnessTexture", "clearcoatNormalTexture"):
                if key in coat:
                    del coat[key]
                    changes.append({"material": material_index, "omitted": key,
                                    "reason": "Use constant clearcoat factors; shared Material has no clearcoat texture slots"})
            infos = []

            def collect(value):
                if isinstance(value, dict):
                    for key, nested in list(value.items()):
                        if key.endswith("Texture") and isinstance(nested, dict) and "index" in nested:
                            transform = nested.get("extensions", {}).get("KHR_texture_transform", {})
                            signature = (transform.get("texCoord", nested.get("texCoord", 0)),
                                         tuple(transform.get("offset", [0, 0])), transform.get("rotation", 0),
                                         tuple(transform.get("scale", [1, 1])))
                            infos.append((value, key, nested, signature))
                        else:
                            collect(nested)

            collect(material)
            if not infos:
                continue
            primary = next((i for i in infos if i[1] == "baseColorTexture"), infos[0])
            chosen = primary[3]
            signatures[material_index] = chosen
            for owner, key, info, signature in infos:
                if signature != chosen:
                    del owner[key]
                    changes.append({"material": material_index, "omitted": key,
                                    "reason": "Different UV set/transform from primary texture; retain primary texture and scalar factors"})
                    continue
                info.pop("texCoord", None)
                info.get("extensions", {}).pop("KHR_texture_transform", None)
                if "extensions" in info and not info["extensions"]:
                    del info["extensions"]
            if chosen != (0, (0, 0), 0, (1, 1)):
                changes.append({"material": material_index, "bakedUVTransform": chosen})
        for mesh in self.doc["meshes"]:
            for primitive in mesh["primitives"]:
                signature = signatures.get(primitive.get("material"))
                if signature is None:
                    continue
                coordinate, offset, angle, scale = signature
                attrs = primitive["attributes"]
                key = "TEXCOORD_" + str(coordinate)
                if key not in attrs:
                    raise ValueError("Primary texture references missing " + key)
                uv = self.accessor(attrs[key]).astype(float) * np.array(scale)
                rotation = np.array([[np.cos(angle), -np.sin(angle)], [np.sin(angle), np.cos(angle)]])
                uv = uv @ rotation.T + np.array(offset)
                attrs["TEXCOORD_0"] = self.append_accessor(uv, "VEC2")
        for key in ("extensionsUsed", "extensionsRequired"):
            if key in self.doc:
                self.doc[key] = [e for e in self.doc[key] if e != "KHR_texture_transform"]
                if not self.doc[key]:
                    del self.doc[key]
        return changes

    def resize_images(self, roles):
        resized = []
        for image_index, (image, role) in enumerate(zip(self.doc.get("images", []), roles)):
            data = self.dependency(image["uri"]) if "uri" in image else self.views[image["bufferView"]]
            with Image.open(io.BytesIO(data)) as original:
                width, height = original.size
                scale = min(1, MAX_TEXTURE / max(width, height))
                size = (max(1, round(width * scale)), max(1, round(height * scale)))
                rgba = np.asarray(original.convert("RGBA"), dtype=np.float32) / 255
            if size != (width, height):
                # 颜色在近似线性光量中缩小；法线先解码、过滤，再归一化；数据贴图不做 sRGB 解码。
                if role == {"normal"}:
                    rgba[:, :, :3] = rgba[:, :, :3] * 2 - 1
                elif role == {"srgb"}:
                    rgb = rgba[:, :, :3]
                    rgba[:, :, :3] = np.where(rgb <= .04045, rgb / 12.92, ((rgb + .055) / 1.055) ** 2.4)
                rgba = np.stack([np.asarray(Image.fromarray(rgba[:, :, channel]).resize(size, Image.Resampling.LANCZOS))
                                 for channel in range(4)], axis=-1)
                if role == {"normal"}:
                    rgba[:, :, :3] = self.normalize(rgba[:, :, :3]) * .5 + .5
                elif role == {"srgb"}:
                    rgb = np.clip(rgba[:, :, :3], 0, 1)
                    rgba[:, :, :3] = np.where(rgb <= .0031308, rgb * 12.92, 1.055 * rgb ** (1 / 2.4) - .055)
            rgba = np.uint8(np.rint(np.clip(rgba, 0, 1) * 255))
            encoded = io.BytesIO()
            Image.fromarray(rgba).save(encoded, format="PNG")
            image.pop("uri", None)
            image["bufferView"] = self.append_view(encoded.getvalue())
            image["mimeType"] = "image/png"
            resized.append({"image": image_index, "original": [width, height], "prepared": list(size), "role": sorted(role)})
        return resized

    def save(self, output):
        doc = self.doc
        used = sorted({index for mesh in doc["meshes"] for primitive in mesh["primitives"]
                       for index in [*primitive["attributes"].values(), *([primitive["indices"]] if "indices" in primitive else [])]})
        remap = {old: new for new, old in enumerate(used)}
        doc["accessors"] = [doc["accessors"][i] for i in used]
        for mesh in doc["meshes"]:
            for primitive in mesh["primitives"]:
                primitive["attributes"] = {key: remap[i] for key, i in primitive["attributes"].items()}
                if "indices" in primitive:
                    primitive["indices"] = remap[primitive["indices"]]
        references = [a for a in doc["accessors"] if "bufferView" in a] + doc.get("images", [])
        for accessor in doc["accessors"]:
            if "sparse" in accessor:
                references.extend([accessor["sparse"]["indices"], accessor["sparse"]["values"]])
        views = sorted({r["bufferView"] for r in references})
        view_map, binary, packed = {}, bytearray(), []
        for old in views:
            view_map[old] = len(packed)
            binary.extend(b"\0" * (-len(binary) % 4))
            view = copy.deepcopy(doc["bufferViews"][old])
            view.update(buffer=0, byteOffset=len(binary), byteLength=len(self.views[old]))
            binary.extend(self.views[old])
            packed.append(view)
        for reference in references:
            reference["bufferView"] = view_map[reference["bufferView"]]
        doc["bufferViews"] = packed
        doc["buffers"] = [{"byteLength": len(binary)}]
        json_data = json.dumps(doc, ensure_ascii=False, separators=(",", ":")).encode("utf-8")
        json_data += b" " * (-len(json_data) % 4)
        binary.extend(b"\0" * (-len(binary) % 4))
        payload = struct.pack("<III", 0x46546C67, 2, 28 + len(json_data) + len(binary))
        payload += struct.pack("<II", len(json_data), 0x4E4F534A) + json_data
        payload += struct.pack("<II", len(binary), 0x004E4942) + binary
        output.parent.mkdir(parents=True, exist_ok=True)
        with output.open("xb") as stream:
            stream.write(payload)


def prepare(relative, group, name):
    source = ROOT / relative
    output = ROOT / "04_ready_static" / group / name / (name + "_static_512.glb")
    if output.exists():
        print("Kept existing:", output.name)
        return
    asset = Asset(source)
    original_extensions = copy.deepcopy(asset.doc.get("extensions", {}))
    vrm_meta = original_extensions.get("VRMC_vrm", original_extensions.get("VRM", {})).get("meta")
    provenance = {
        "original": relative, "generatedBy": "scripts/prepare-model-library.py",
        "purpose": "Local static import sample; no engine/import/render validation performed",
        "changes": ["Bake default-pose skin and default morph weights; remove animation/morph controls",
                    "Use ordinary glTF PBR fallback; remove special VRM/MToon/spring/constraint and unsupported material extensions",
                    "Resize retained textures to at most 512px; package as self-contained GLB"],
        "sourceVRMMeta": vrm_meta,
        "licenseNotes": "Original asset license remains in force; see ../../../sources and original asset folder. Not relicensed as CC0.",
    }
    asset.bake_static()
    asset.remove_special_extensions(asset.doc)
    for key in ("extensionsUsed", "extensionsRequired"):
        if key in asset.doc:
            asset.doc[key] = [e for e in asset.doc[key] if e in KEPT_EXTENSIONS]
            if not asset.doc[key]:
                del asset.doc[key]
    provenance["materialCompatibilityChanges"] = asset.adapt_single_uv_materials()
    roles = asset.material_textures()
    provenance["images"] = asset.resize_images(roles)
    asset.doc.setdefault("asset", {}).setdefault("extras", {})["EmberframeAssetPreparation"] = provenance
    asset.save(output)
    output.with_name("SOURCE.json").write_text(json.dumps(provenance, ensure_ascii=False, indent=2), encoding="utf-8")
    print("Prepared:", output.relative_to(ROOT), f"({output.stat().st_size / 1048576:.2f} MiB)")


if __name__ == "__main__":
    for name in ("Avocado", "WaterBottle", "BoomBox", "Lantern", "AntiqueCamera", "ToyCar", "SheenChair"):
        prepare(f"03_realistic/{name}/{name}.glb", "realistic", name)
    prepare("03_realistic/FlightHelmet/glTF/FlightHelmet.gltf", "realistic", "FlightHelmet")
    for relative, name in (
        ("AvatarSample_B/AvatarSample_B.vrm", "AvatarSample_B"),
        ("ConstraintTwist/VRM1_Constraint_Twist_Sample.vrm", "ConstraintTwist"),
        ("AliciaSolid/AliciaSolid_vrm-0.51.vrm", "AliciaSolid"),
        ("SeedSan/Seed-san.vrm", "SeedSan"),
    ):
        prepare("01_anime/" + relative, "anime", name)
    print("Asset preparation only. No engine, build, rendering or tests were started.")
