#include "editor_gizmo.h"
#include "editor_history.h"
#include "editor_transform.h"
#include "editor_camera.h"
#include "editor_objects.h"

namespace emberframe::lab {
TestResults test_editor_gizmo() {
    using namespace editor;TestResults r;
    const auto rect=image_rect(224,70,800,500,640,360);
    r.push_back({"图像留边与坐标",std::abs(rect.width-800)<.001f&&std::abs(rect.height-450)<.001f&&std::abs(rect.y-95)<.001f,"实际渲染图而非窗口坐标"});
    Camera camera;camera.position={0,0,5};camera.target={0,0,0};
    const ViewportRect view{100,50,800,450};
    for(bool reversed:{false,true}) {
        const auto p=project_handle({.7f,.2f,0},camera,view,reversed);
        const auto ray=handle_ray(*p,camera,view,reversed);
        const auto t=axis_parameter(ray,{0,.2f,0},{1,0,0});
        r.push_back({reversed?"Reversed-Z 拖动射线":"普通深度拖动射线",t&&std::abs(*t-.7f)<.0001f,"屏幕投影后仍还原同一世界坐标轴参数"});
    }
    r.push_back({"轴平行镜头不会跳变",!axis_parameter({{0,0,5},{0,0,-1}},glm::vec3(0),{0,0,1}),"拒绝不稳定的最近点求解"});
    r.push_back({"镜头后方不显示工具",!project_handle({0,0,6},camera,view,false),"不穿越近平面"});
    r.push_back({"鼠标命中线段",std::abs(segment_distance({3,2},{0,0},{4,0})-2)<.0001f&&std::abs(segment_distance({6,0},{0,0},{4,0})-2)<.0001f,"工具拾取使用线段而非无限直线"});
    Scene scene=make_demo_scene(1);Settings settings;Camera history_camera=camera;UndoStack history(16,16*1024*1024);
    auto before=std::make_shared<Snapshot>(Snapshot{scene,history_camera,settings});
    scene.lights[0].intensity=7;++scene.revision;
    record_snapshot(history,"二进制场景快照",before,scene,history_camera,settings);
    const auto revision=scene.revision;
    const bool undo_ok=history.undo()&&scene.lights[0].intensity==before->scene.lights[0].intensity&&scene.revision>revision;
    const bool redo_ok=history.redo()&&scene.lights[0].intensity==7;
    r.push_back({"编辑快照撤销重做",undo_ok&&redo_ok,"恢复内容且保持 revision 递增"});
    const auto previous_bytes=scene_bytes(scene);Texture texture;texture.levels.emplace_back(128,128,glm::vec4(.5f));scene.textures.push_back(std::move(texture));
    const auto added_bytes=scene_bytes(scene)-previous_bytes;
    r.push_back({"快照按像素内存计费",added_bytes>=128*128*sizeof(glm::vec4)&&added_bytes<128*128*sizeof(glm::vec4)+4096,"不使用十进制纹理序列化作撤销快照"});
    auto matrix_close=[](const glm::mat4& a,const glm::mat4& b){
        for(int c=0;c<4;++c)for(int row=0;row<4;++row)if(std::abs(a[c][row]-b[c][row])>2e-4f)return false;
        return true;
    };
    bool transform_roundtrip=true;
    for(float pitch:{-90.f,-35.f,0.f,42.f,90.f})for(float mirror:{-1.f,1.f}){
        LocalTransform pose;pose.position={2,-3,4};pose.rotation_degrees={17,pitch,-28};
        pose.scale={mirror*1.2f,.8f,2.1f};pose.shear={.1f,-.2f,.15f};
        const auto matrix=compose_local_transform(pose);
        transform_roundtrip&=matrix_close(matrix,compose_local_transform(local_transform(matrix)));
    }
    r.push_back({"旋转缩放检查器保留镜像剪切及万向节边界",transform_roundtrip,"十组含正负缩放及 ±90° 欧拉角的分解重组"});
    Scene hierarchy;hierarchy.nodes.resize(2);hierarchy.nodes[1].parent=0;
    LocalTransform parent_pose;parent_pose.position={3,4,-2};parent_pose.rotation_degrees={20,-15,32};parent_pose.scale={-2,1,3};
    hierarchy.nodes[0].local=compose_local_transform(parent_pose);
    LocalTransform child_pose;child_pose.position={-1,2,3};child_pose.rotation_degrees={-12,35,9};child_pose.scale={.6f,1.3f,.9f};
    const auto requested_world=compose_local_transform(child_pose);const auto old_parent=hierarchy.nodes[0].local;
    set_node_world(hierarchy,1,requested_world);
    r.push_back({"嵌套模型世界旋转缩放转回局部变换",matrix_close(scene_world_transforms(hierarchy)[1],requested_world)&&matrix_close(hierarchy.nodes[0].local,old_parent),"逆父矩阵保留非均匀与镜像父变换，不移动父节点"});
    const auto valid_local=hierarchy.nodes[1].local;const auto valid_revision=hierarchy.revision;bool rejected=false;
    try{auto bad=valid_local;bad[0]=glm::vec4(0);set_node_local(hierarchy,1,bad);}catch(const std::invalid_argument&){rejected=true;}
    r.push_back({"零缩放拒绝且不破坏已显示模型",rejected&&hierarchy.revision==valid_revision&&matrix_close(hierarchy.nodes[1].local,valid_local),"错误发生在写入节点前，模型与 revision 均保持不变"});
    const auto ring=ring_direction({{1,0,5},{0,0,-1}},glm::vec3(0),{0,0,1});
    r.push_back({"旋转圆环射线求交与平行拒绝",ring&&glm::length(*ring-glm::vec3(1,0,0))<1e-5f&&!ring_direction({{1,0,5},{1,0,0}},glm::vec3(0),{0,0,1}),"命中方向用于求旋转角，掠射圆环不跳变"});
    UndoStack transform_history(8,16*1024*1024);Camera transform_camera;Settings transform_settings;
    transform_history.begin(hierarchy,transform_camera,transform_settings,"旋转缩放");
    auto changed=local_transform(hierarchy.nodes[1].local);changed.rotation_degrees.y+=35;changed.scale*=1.5f;
    set_node_local(hierarchy,1,compose_local_transform(changed));const auto edited=hierarchy.nodes[1].local;transform_history.commit();
    const bool transform_undo=transform_history.undo()&&matrix_close(hierarchy.nodes[1].local,valid_local);
    const bool transform_redo=transform_history.redo()&&matrix_close(hierarchy.nodes[1].local,edited);
    r.push_back({"模型旋转缩放撤销重做",transform_undo&&transform_redo,"复用实际编辑历史，恢复完整节点矩阵"});
    bool framing=true;
    for(float scale:{1e-4f,1.f,1e4f}){
        Aabb bounds;bounds.expand(glm::vec3(-scale));bounds.expand(glm::vec3(scale));
        Camera fitted;frame_bounds(bounds,fitted);
        const float radius=glm::length(bounds.extent())*.5f;
        framing&=glm::length(fitted.position-fitted.target)/radius>2.5f&&glm::length(fitted.position-fitted.target)/radius<2.7f;
        framing&=std::abs(fitted.near_plane/radius-.002f)<1e-6f&&fitted.far_plane>glm::length(fitted.position-fitted.target)+radius;
    }
    r.push_back({"微小及大型模型按实际尺寸聚焦",framing,"三种相差八个数量级的单位保持相同画面占比，裁剪面同步调整"});
    Scene objects;const int cube=create_object(objects,NewObject::cube,-1,{2,1,3});
    const int sphere=create_object(objects,NewObject::sphere,cube);
    const int plane=create_object(objects,NewObject::plane);
    const int empty=create_object(objects,NewObject::empty);
    r.push_back({"右键模型创建使用真实网格和节点",objects.nodes.size()==4&&objects.meshes.size()==3&&objects.nodes[sphere].parent==cube&&objects.nodes[empty].mesh==-1&&objects.meshes[objects.nodes[plane].mesh].indices.size()==6&&!flatten_scene(objects).empty(),"立方体、球体、平面与空节点走普通场景路径"});
    auto instance=objects.nodes[cube];instance.name="共享立方体";const int other=int(objects.nodes.size());objects.nodes.push_back(instance);
    const int shared_mesh=objects.nodes[cube].mesh,shared_material=node_material(objects,cube,0);
    const auto old_shared=objects.materials[shared_material];const int independent=make_node_material_unique(objects,cube,0);
    objects.materials[independent].roughness=.12f;
    r.push_back({"节点独立材质不污染共享实例",objects.nodes[cube].mesh!=shared_mesh&&objects.nodes[other].mesh==shared_mesh&&node_material(objects,other,0)==shared_material&&objects.materials[shared_material].roughness==old_shared.roughness&&node_material(objects,cube,0)==independent,"复制材质并按需隔离网格引用，保留旧资源"});
    const auto before_rotation=scene_world_transforms(objects)[sphere];
    const auto requested_rotation=drag_rotation(before_rotation,{45,-25},camera,false);
    set_node_world(objects,sphere,requested_rotation);
    const auto after_rotation=scene_world_transforms(objects)[sphere];
    r.push_back({"Ctrl 拖动旋转保留实例位置及父节点",matrix_close(after_rotation,requested_rotation)&&glm::length(glm::vec3(after_rotation[3])-glm::vec3(before_rotation[3]))<1e-4f&&objects.nodes[cube].local[3]==glm::vec4(2,1,3,1),"世界轴旋转结果逆父矩阵写回局部，不移动实例原点"});
    r.push_back({"零鼠标位移不旋转",matrix_close(drag_rotation(before_rotation,{0,0},camera,false),before_rotation),"不产生首帧跳变"});
    return r;
}
}
