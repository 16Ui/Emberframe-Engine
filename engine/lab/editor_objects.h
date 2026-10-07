#pragma once
#include "editor_transform.h"

namespace emberframe::lab::editor {
enum class PropertyTarget { none,node,light,mesh,material,texture };
enum class NewObject { empty,cube,sphere,plane };

inline std::size_t mesh_instance_count(const Scene& scene,int mesh) {
    return std::count_if(scene.nodes.begin(),scene.nodes.end(),[&](const Node& node){return node.mesh==mesh;});
}
inline void changed_objects(Scene& scene) {
    if(scene.revision==UINT64_MAX)throw std::overflow_error("Scene revision exhausted");
    scene.baked_resources.reset();scene.asset_revision=0;++scene.revision;
}

// 创建的模型是普通网格资源 + 普通场景节点，使用和导入模型相同的渲染路径。
inline int create_object(Scene& scene,NewObject kind,int parent=-1,glm::vec3 root_position={0,0,0}) {
    if(parent<-1||parent>=int(scene.nodes.size()))throw std::out_of_range("无效父节点");
    if(scene.revision==UINT64_MAX)throw std::overflow_error("Scene revision exhausted");
    // 兼容旧工程“无节点时每份网格隐式实例化”的约定，首次新建不能让旧模型消失。
    if(scene.nodes.empty())for(int m=0;m<int(scene.meshes.size());++m){Node existing;existing.name=scene.meshes[m].name;existing.mesh=m;scene.nodes.push_back(std::move(existing));}
    Node node;node.parent=parent;
    node.name=kind==NewObject::cube?"立方体":kind==NewObject::sphere?"球体":kind==NewObject::plane?"平面":"空节点";
    node.name+=" "+std::to_string(scene.nodes.size()+1);
    if(parent<0)node.local=glm::translate(glm::mat4(1),root_position);
    if(kind!=NewObject::empty){
        Mesh mesh;mesh.name=node.name+" · 网格";
        auto vertex=[&](glm::vec3 p,glm::vec3 n,glm::vec2 uv,glm::vec4 tangent){
            Vertex v;v.position=p;v.normal=n;v.uv=uv;v.tangent=tangent;mesh.vertices.push_back(v);
        };
        if(kind==NewObject::sphere){
            constexpr int rows=24,columns=32;
            for(int y=0;y<=rows;++y)for(int x=0;x<=columns;++x){
                const float a=pi*float(y)/rows,b=2*pi*float(x)/columns;
                const glm::vec3 n{std::sin(a)*std::cos(b),std::cos(a),std::sin(a)*std::sin(b)};
                vertex(n*.5f,n,{float(x)/columns,float(y)/rows},{-std::sin(b),0,std::cos(b),1});
            }
            for(int y=0;y<rows;++y)for(int x=0;x<columns;++x){
                const auto a=std::uint32_t(y*(columns+1)+x),b=a+columns+1;
                if(y>0)mesh.indices.insert(mesh.indices.end(),{a,a+1,b});
                if(y<rows-1)mesh.indices.insert(mesh.indices.end(),{a+1,b+1,b});
            }
        }else{
            auto face=[&](glm::vec3 center,glm::vec3 u,glm::vec3 v,glm::vec3 n){
                const auto first=std::uint32_t(mesh.vertices.size());
                vertex(center-u*.5f-v*.5f,n,{0,0},glm::vec4(u,1));
                vertex(center+u*.5f-v*.5f,n,{1,0},glm::vec4(u,1));
                vertex(center+u*.5f+v*.5f,n,{1,1},glm::vec4(u,1));
                vertex(center-u*.5f+v*.5f,n,{0,1},glm::vec4(u,1));
                mesh.indices.insert(mesh.indices.end(),{first,first+1,first+2,first,first+2,first+3});
            };
            if(kind==NewObject::plane)face({0,0,0},{1,0,0},{0,0,-1},{0,1,0});
            else{
                face({.5f,0,0},{0,0,-1},{0,1,0},{1,0,0});
                face({-.5f,0,0},{0,0,1},{0,1,0},{-1,0,0});
                face({0,.5f,0},{1,0,0},{0,0,-1},{0,1,0});
                face({0,-.5f,0},{1,0,0},{0,0,1},{0,-1,0});
                face({0,0,.5f},{1,0,0},{0,1,0},{0,0,1});
                face({0,0,-.5f},{-1,0,0},{0,1,0},{0,0,-1});
            }
        }
        Material material;material.name=node.name+" · 材质";
        mesh.primitives.push_back({0,std::uint32_t(mesh.indices.size()),std::uint32_t(scene.materials.size())});
        node.mesh=int(scene.meshes.size());scene.materials.push_back(std::move(material));scene.meshes.push_back(std::move(mesh));
    }
    const int index=int(scene.nodes.size());scene.nodes.push_back(std::move(node));changed_objects(scene);return index;
}

inline int node_material(const Scene& scene,int node,std::size_t primitive) {
    const auto& mesh=scene.meshes.at(scene.nodes.at(node).mesh);
    if(mesh.primitives.empty()){if(primitive!=0)throw std::out_of_range("无效网格分部");return scene.materials.empty()?-1:0;}
    return int(mesh.primitives.at(primitive).material);
}

// 材质引用原本保存在 Mesh，而不是 Node。先按需复制网格，再改引用，
// 这样“给这个实例换材质”不会意外改变另一个引用同一网格的节点。
inline void assign_node_material(Scene& scene,int node,std::size_t primitive,int material) {
    if(scene.revision==UINT64_MAX)throw std::overflow_error("Scene revision exhausted");
    if(material<0||material>=int(scene.materials.size()))throw std::out_of_range("无效材质");
    const int old_mesh=scene.nodes.at(node).mesh;const auto& original=scene.meshes.at(old_mesh);
    if(primitive>=std::max<std::size_t>(1,original.primitives.size()))throw std::out_of_range("无效网格分部");
    Mesh mesh=original;
    if(mesh.primitives.empty())mesh.primitives.push_back({0,std::uint32_t(mesh.indices.size()),0});
    mesh.primitives.at(primitive).material=std::uint32_t(material);
    // 派生 LOD 可能合并了分部，不能沿用失配的材质映射；原始几何完全保留。
    mesh.lods.clear();
    if(mesh_instance_count(scene,old_mesh)>1){
        mesh.name+=" · 独立实例";const int next=int(scene.meshes.size());scene.meshes.push_back(std::move(mesh));scene.nodes.at(node).mesh=next;
    }else scene.meshes.at(old_mesh)=std::move(mesh);
    changed_objects(scene);
}
inline int make_node_material_unique(Scene& scene,int node,std::size_t primitive) {
    const int source=node_material(scene,node,primitive);
    Material material=source>=0?scene.materials.at(source):Material{};
    material.name+=" · "+scene.nodes.at(node).name;
    const int index=int(scene.materials.size());scene.materials.push_back(std::move(material));
    assign_node_material(scene,node,primitive,index);return index;
}

// 从初始矩阵计算拖动结果而非每帧累乘；旋转围绕实例原点，保留位置、缩放和剪切。
inline glm::mat4 drag_rotation(const glm::mat4& initial,glm::vec2 delta,const Camera& camera,bool snap) {
    glm::vec2 angle=delta*.008f;
    if(snap)for(int i=0;i<2;++i)angle[i]=std::round(angle[i]/(pi/12))*(pi/12);
    const glm::vec3 right=safe_normalize(glm::vec3(glm::inverse(camera.view())[0]),{1,0,0});
    auto rotation=glm::rotate(glm::mat4(1),angle.x,{0,1,0})*glm::rotate(glm::mat4(1),angle.y,right);
    const glm::vec3 position(initial[3]);
    return glm::translate(glm::mat4(1),position)*rotation*glm::translate(glm::mat4(1),-position)*initial;
}
} // namespace emberframe::lab::editor
