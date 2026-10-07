#include "mesh_lod.h"
#include <cmath>
#include <map>
#include <set>
#include <utility>

namespace emberframe::lab {
namespace {
template<glm::length_t N, class T, glm::qualifier Q>
bool finite(const glm::vec<N,T,Q>& v) {
    for(glm::length_t i=0;i<N;++i) if(!std::isfinite(v[i])) return false;
    return true;
}
glm::vec3 unit(glm::dvec3 v,glm::vec3 fallback={0,1,0}) {
    const double q=glm::dot(v,v);
    return q>0 && std::isfinite(q)?glm::vec3(v/std::sqrt(q)):fallback;
}
glm::vec3 orthogonal(glm::vec3 n) {
    return unit(glm::cross(glm::dvec3(n),std::abs(n.x)<0.8f?glm::dvec3(1,0,0):glm::dvec3(0,1,0)),{1,0,0});
}
glm::vec4 tangent_frame(glm::vec4 tangent,glm::vec3 normal) {
    const glm::dvec3 t(tangent);
    return {unit(t-glm::dvec3(normal)*glm::dot(t,glm::dvec3(normal)),orthogonal(normal)),tangent.w<0?-1.0f:1.0f};
}
template<glm::length_t N>
glm::vec<N,float> interpolate(const glm::vec<N,float>& a,const glm::vec<N,float>& b,double t) {
    return glm::vec<N,float>(glm::vec<N,double>(a)*(1-t)+glm::vec<N,double>(b)*t);
}
}

namespace {
struct QemFace { std::array<std::uint32_t,3> vertices; std::uint32_t material=0; bool active=true; };
struct QemEdge { std::uint32_t faces=0; int winding=0; };
struct QemGraph {
    std::map<std::pair<std::uint32_t,std::uint32_t>,QemEdge> edges;
    std::vector<std::set<std::uint32_t>> neighbors;
    std::vector<std::vector<std::size_t>> incident;
    std::vector<bool> locked;
};
QemGraph qem_graph(const std::vector<QemFace>& faces,std::size_t vertex_count) {
    QemGraph graph; graph.neighbors.resize(vertex_count); graph.incident.resize(vertex_count); graph.locked.resize(vertex_count,false);
    std::vector<std::int64_t> material(vertex_count,-1);
    for(std::size_t id=0;id<faces.size();++id) if(faces[id].active) {
        const auto& face=faces[id];
        for(int k=0;k<3;++k) {
            const auto a=face.vertices[k],b=face.vertices[(k+1)%3];
            graph.incident[a].push_back(id); graph.neighbors[a].insert(b); graph.neighbors[b].insert(a);
            auto& edge=graph.edges[std::minmax(a,b)]; ++edge.faces; edge.winding+=a<b?1:-1;
            if(material[a]<0) material[a]=face.material;
            else if(material[a]!=face.material) graph.locked[a]=true;
        }
    }
    for(const auto& [vertices,edge]:graph.edges) {
        if(edge.faces>2 || (edge.faces==2 && edge.winding!=0))
            throw std::invalid_argument("QEM requires manifold, consistently wound edges");
        if(edge.faces==1) { graph.locked[vertices.first]=true; graph.locked[vertices.second]=true; }
    }
    return graph;
}
void check_qem_fans(const std::vector<QemFace>& faces,const QemGraph& graph) {
    for(std::size_t vertex=0;vertex<graph.incident.size();++vertex) {
        if(graph.incident[vertex].empty()) continue;
        std::map<std::uint32_t,std::vector<std::uint32_t>> fan;
        for(const auto id:graph.incident[vertex]) {
            const auto& face=faces[id]; std::array<std::uint32_t,2> opposite{}; int count=0;
            for(const auto v:face.vertices) if(v!=vertex) opposite[std::size_t(count++)]=v;
            if(count!=2) throw std::invalid_argument("QEM degenerate index triple");
            fan[opposite[0]].push_back(opposite[1]); fan[opposite[1]].push_back(opposite[0]);
        }
        int endpoints=0;
        for(const auto& [v,neighbors]:fan) {
            (void)v;
            if(neighbors.size()==1) ++endpoints;
            else if(neighbors.size()!=2) throw std::invalid_argument("QEM nonmanifold vertex fan");
        }
        if(endpoints!=0 && endpoints!=2) throw std::invalid_argument("QEM nonmanifold vertex boundary");
        std::set<std::uint32_t> visited; std::vector<std::uint32_t> stack{fan.begin()->first};
        while(!stack.empty()) {
            const auto v=stack.back(); stack.pop_back(); if(!visited.insert(v).second) continue;
            for(const auto neighbor:fan.at(v)) stack.push_back(neighbor);
        }
        if(visited.size()!=fan.size()) throw std::invalid_argument("QEM disconnected vertex fan");
    }
}
}
QemResult simplify_qem(const Mesh& mesh,QemOptions options) {
    if(std::isnan(options.max_error) || options.max_error<0 || options.max_collapses>4096)
        throw std::invalid_argument("Invalid bounded QEM options");
    std::vector<Vertex> vertices;
    std::map<std::uint32_t,std::uint32_t> remap;
    std::vector<QemFace> faces;
    auto append=[&](std::size_t first,std::size_t count,std::uint32_t material) {
        const std::size_t available=mesh.indices.empty()?mesh.vertices.size():mesh.indices.size();
        if(count%3 || first>available || count>available-first) throw std::invalid_argument("Invalid QEM triangle list");
        if(count/3>4096-faces.size()) throw std::invalid_argument("Reference QEM supports at most 4096 triangles");
        for(std::size_t i=first;i<first+count;i+=3) {
            QemFace face; face.material=material;
            for(int k=0;k<3;++k) {
                const std::size_t original=mesh.indices.empty()?i+k:mesh.indices[i+k];
                if(original>=mesh.vertices.size() || original>std::numeric_limits<std::uint32_t>::max())
                    throw std::invalid_argument("QEM index out of range");
                const auto& v=mesh.vertices[original];
                if(!finite(v.position) || !finite(v.normal) || !finite(v.tangent) || !finite(v.uv) || !finite(v.color) || !finite(v.baked_irradiance))
                    throw std::invalid_argument("Non-finite QEM vertex");
                const auto [it,inserted]=remap.emplace(std::uint32_t(original),std::uint32_t(vertices.size()));
                if(inserted) vertices.push_back(v);
                face.vertices[k]=it->second;
            }
            faces.push_back(face);
        }
    };
    if(mesh.primitives.empty()) append(0,mesh.indices.empty()?mesh.vertices.size():mesh.indices.size(),0);
    else for(const auto& primitive:mesh.primitives) append(primitive.first_index,primitive.index_count,primitive.material);
    QemResult result; result.input_triangles=faces.size(); result.output_triangles=faces.size(); result.mesh.name=mesh.name+" (QEM)";
    if(faces.empty()) {
        result.target_reached=true; result.stop_reason=QemStopReason::target_reached;
        result.reason="Empty input already satisfies the triangle budget"; return result;
    }
    Aabb bounds; for(const auto& v:vertices) bounds.expand(v.position);
    const glm::dvec3 center=bounds.center(),extent=glm::dvec3(bounds.max)-glm::dvec3(bounds.min);
    const double scale=std::max({extent.x,extent.y,extent.z});
    if(!(scale>0)) throw std::invalid_argument("QEM has no nondegenerate geometry");
    std::vector<glm::dvec3> positions; positions.reserve(vertices.size());
    for(const auto& v:vertices) positions.push_back((glm::dvec3(v.position)-center)/scale);
    std::vector<glm::dmat4> quadrics(vertices.size(),glm::dmat4(0));
    std::vector<double> displacement(vertices.size(),0);
    const auto original_positions=positions;
    std::vector<std::vector<std::size_t>> represented(vertices.size());
    for(std::size_t i=0;i<represented.size();++i) represented[i].push_back(i);
    std::vector<bool> seam_locked(vertices.size(),false);
    std::map<std::array<float,3>,std::vector<std::size_t>> coincident;
    for(std::size_t i=0;i<vertices.size();++i) {
        const auto p=vertices[i].position; coincident[{p.x,p.y,p.z}].push_back(i);
    }
    for(const auto& [position,ids]:coincident) {
        (void)position; bool split=false;
        for(const auto i:ids) if(vertices[i].uv!=vertices[ids.front()].uv ||
                                  vertices[i].normal!=vertices[ids.front()].normal ||
                                  vertices[i].tangent.w!=vertices[ids.front()].tangent.w) split=true;
        if(split) for(const auto i:ids) seam_locked[i]=true;
    }
    std::set<std::array<std::uint32_t,3>> unique_faces;
    for(const auto& face:faces) {
        auto key=face.vertices; std::sort(key.begin(),key.end());
        if(key[0]==key[1] || key[1]==key[2] || !unique_faces.insert(key).second) throw std::invalid_argument("QEM duplicate/degenerate face");
        const auto a=positions[face.vertices[0]],b=positions[face.vertices[1]],c=positions[face.vertices[2]];
        const auto cross=glm::cross(b-a,c-a); const double length=glm::length(cross);
        if(!(length>0)) throw std::invalid_argument("QEM zero-area face");
        const auto normal=cross/length; const glm::dvec4 plane(normal,-glm::dot(normal,a));
        // 平面距离平方写成齐次二次型 p^T Q p；坐标归一化改善求逆条件，误差再换回世界单位。
        const auto q=glm::outerProduct(plane,plane);
        for(const auto v:face.vertices) quadrics[v]+=q;
    }
    auto graph=qem_graph(faces,vertices.size()); check_qem_fans(faces,graph);
    bool error_blocked=false;
    while(result.output_triangles>options.target_triangles && result.collapses<options.max_collapses) {
        error_blocked=false;
        bool found=false; double best_error=std::numeric_limits<double>::infinity();
        std::uint32_t best_a=0,best_b=0; glm::dvec3 best_position(0);
        for(const auto& [edge,info]:graph.edges) {
            const auto [a,b]=edge;
            if(info.faces!=2 || graph.locked[a] || graph.locked[b] || seam_locked[a] || seam_locked[b] ||
               (vertices[a].tangent.w<0)!=(vertices[b].tangent.w<0)) { ++result.locked_edge_tests; continue; }
            std::set<std::uint32_t> common,opposite;
            for(const auto v:graph.neighbors[a]) if(graph.neighbors[b].contains(v)) common.insert(v);
            std::vector<std::size_t> affected=graph.incident[a];
            affected.insert(affected.end(),graph.incident[b].begin(),graph.incident[b].end());
            std::sort(affected.begin(),affected.end()); affected.erase(std::unique(affected.begin(),affected.end()),affected.end());
            bool topology_valid=true; std::set<std::array<std::uint32_t,3>> replacement_faces;
            for(const auto id:affected) {
                const auto& face=faces[id]; bool has_a=false,has_b=false;
                for(const auto v:face.vertices) { has_a|=v==a; has_b|=v==b; }
                if(has_a && has_b) { for(const auto v:face.vertices) if(v!=a && v!=b) opposite.insert(v); continue; }
                auto key=face.vertices; for(auto& v:key) if(v==b) v=a;
                std::sort(key.begin(),key.end());
                if(!replacement_faces.insert(key).second) { topology_valid=false; break; }
            }
            // Link condition：端点的一环交集只能是被删除的两张面的对顶点，防止产生非流形连接。
            if(!topology_valid || common!=opposite || opposite.size()!=2) { ++result.topology_rejections; continue; }
            const glm::dmat4 q=quadrics[a]+quadrics[b]; const glm::dmat3 system(q);
            double matrix_scale=0; for(int i=0;i<3;++i) for(int j=0;j<3;++j) matrix_scale=std::max(matrix_scale,std::abs(system[i][j]));
            std::vector<glm::dvec3> candidates{(positions[a]+positions[b])*0.5,positions[a],positions[b]};
            if(matrix_scale>0 && std::abs(glm::determinant(system))>1e-12*matrix_scale*matrix_scale*matrix_scale) {
                const auto optimum=-glm::inverse(system)*glm::dvec3(q[3]);
                if(finite(optimum)) candidates.insert(candidates.begin(),optimum);
            }
            for(auto candidate:candidates) {
                // 实际输出为 float，先量化再检查翻面，避免只在双精度临时几何上验证成功。
                const glm::vec3 output_position(candidate*scale+center);
                if(!finite(output_position)) continue;
                candidate=(glm::dvec3(output_position)-center)/scale;
                const glm::dvec4 h(candidate,1);
                const double error=std::max(0.0,glm::dot(h,q*h))*scale*scale;
                if(!std::isfinite(error)) continue;
                if(error>options.max_error) { error_blocked=true; continue; }
                if(found && error>=best_error) continue;
                bool legal=true;
                for(const auto id:affected) {
                    const auto& face=faces[id]; bool has_a=false,has_b=false;
                    for(const auto v:face.vertices) { has_a|=v==a; has_b|=v==b; }
                    if(has_a && has_b) continue;
                    std::array<glm::dvec3,3> before,after;
                    for(int k=0;k<3;++k) { const auto v=face.vertices[k]; before[k]=positions[v]; after[k]=(v==a || v==b)?candidate:before[k]; }
                    const auto old_normal=glm::cross(before[1]-before[0],before[2]-before[0]);
                    const auto new_normal=glm::cross(after[1]-after[0],after[2]-after[0]);
                    const double old_area2=glm::dot(old_normal,old_normal),new_area2=glm::dot(new_normal,new_normal);
                    if(new_area2<=old_area2*1e-20 || glm::dot(old_normal,new_normal)<0.5*std::sqrt(old_area2*new_area2)) {
                        ++result.flip_rejections; legal=false; break;
                    }
                }
                if(legal) { found=true; best_error=error; best_a=a; best_b=b; best_position=candidate; }
            }
        }
        if(!found) break;
        const auto edge=positions[best_b]-positions[best_a]; const double edge_length2=glm::dot(edge,edge);
        const double fraction=edge_length2>0?std::clamp(glm::dot(best_position-positions[best_a],edge)/edge_length2,0.0,1.0):0.5;
        Vertex merged;
        merged.position=glm::vec3(best_position*scale+center);
        merged.normal=unit(glm::dvec3(interpolate(vertices[best_a].normal,vertices[best_b].normal,fraction)),vertices[best_a].normal);
        merged.uv=interpolate(vertices[best_a].uv,vertices[best_b].uv,fraction);
        merged.color=interpolate(vertices[best_a].color,vertices[best_b].color,fraction);
        // 烘焙辐照度按与 UV/颜色相同的折叠对应关系插值，切换 LOD 时保持光照属性。
        merged.baked_irradiance=interpolate(vertices[best_a].baked_irradiance,vertices[best_b].baked_irradiance,fraction);
        merged.tangent=tangent_frame(interpolate(vertices[best_a].tangent,vertices[best_b].tangent,fraction),merged.normal);
        // 直接跟踪折叠簇中每个原始顶点到代表点的位移，避免累计移动路程使屏幕误差过度膨胀。
        // 即使平面QEM为0，也不能据此伪报位移误差为0。
        represented[best_a].insert(represented[best_a].end(),represented[best_b].begin(),represented[best_b].end());
        represented[best_b].clear(); displacement[best_a]=0; displacement[best_b]=0;
        for(const auto original:represented[best_a])
            displacement[best_a]=std::max(displacement[best_a],glm::length(best_position-original_positions[original]));
        vertices[best_a]=merged; positions[best_a]=best_position; quadrics[best_a]+=quadrics[best_b];
        for(auto& face:faces) if(face.active) {
            for(auto& v:face.vertices) if(v==best_b) v=best_a;
            const auto& v=face.vertices;
            if(v[0]==v[1] || v[1]==v[2] || v[0]==v[2]) { face.active=false; --result.output_triangles; }
        }
        ++result.collapses; result.max_collapse_error=std::max(result.max_collapse_error,best_error);
        graph=qem_graph(faces,vertices.size());
    }
    std::vector<std::uint32_t> output_index(vertices.size(),std::numeric_limits<std::uint32_t>::max());
    for(const auto& face:faces) if(face.active) {
        if(result.mesh.primitives.empty() || result.mesh.primitives.back().material!=face.material)
            result.mesh.primitives.push_back({std::uint32_t(result.mesh.indices.size()),0,face.material});
        result.mesh.primitives.back().index_count+=3;
        for(const auto v:face.vertices) {
            if(output_index[v]==std::numeric_limits<std::uint32_t>::max()) {
                output_index[v]=std::uint32_t(result.mesh.vertices.size()); result.mesh.vertices.push_back(vertices[v]);
            }
            result.mesh.indices.push_back(output_index[v]);
        }
    }
    result.target_reached=result.output_triangles<=options.target_triangles;
    result.geometric_error=*std::max_element(displacement.begin(),displacement.end())*scale;
    if(result.target_reached) {
        result.stop_reason=QemStopReason::target_reached; result.reason="Requested triangle budget reached";
    } else if(result.collapses>=options.max_collapses) {
        result.stop_reason=QemStopReason::collapse_budget; result.reason="Collapse budget exhausted before the triangle target";
    } else if(error_blocked) {
        result.stop_reason=QemStopReason::error_budget; result.reason="Remaining candidate plane errors exceed max_error, or are constrained";
    } else {
        result.stop_reason=QemStopReason::topology_or_seams;
        result.reason="No admissible edge remains: locked boundary/material/UV seam, topology or face-normal constraint";
    }
    return result;
}

LodChain build_lod_chain(const Mesh& mesh,LodBuildOptions options) {
    if(options.triangle_ratios.size()>8) throw std::invalid_argument("At most eight reduced LOD requests");
    float previous=1;
    for(const float ratio:options.triangle_ratios) {
        if(!std::isfinite(ratio) || ratio<=0 || ratio>=previous) throw std::invalid_argument("LOD ratios must strictly decrease within (0,1)");
        previous=ratio;
    }
    // 用零次折叠验证并规范化索引/primitive布局，level 0 不改变任何顶点位置。
    auto full=simplify_qem(mesh,{std::numeric_limits<std::size_t>::max(),0,options.max_qem_error});
    if(options.max_collapses_per_level>4096) throw std::invalid_argument("Invalid LOD collapse budget");
    LodChain chain;
    for(const auto& v:full.mesh.vertices) chain.bounds.expand(v.position);
    chain.levels.push_back({std::move(full.mesh),full.output_triangles,0});
    const std::size_t original=full.output_triangles;
    for(const float ratio:options.triangle_ratios) {
        const auto target=std::size_t(std::ceil(double(original)*ratio));
        auto reduced=simplify_qem(chain.levels.back().mesh,{target,options.max_collapses_per_level,options.max_qem_error});
        const bool appended=reduced.output_triangles<chain.levels.back().triangles;
        chain.attempts.push_back({target,reduced.output_triangles,reduced.collapses,appended,reduced.stop_reason,reduced.reason});
        if(appended) {
            const double error=chain.levels.back().geometric_error+reduced.geometric_error;
            chain.levels.push_back({std::move(reduced.mesh),reduced.output_triangles,error});
        }
    }
    return chain;
}

Mesh build_mesh_lods(Mesh mesh,LodBuildOptions options,MeshLodBuildReport* report) {
    MeshLodBuildReport result;
    const auto available=mesh.indices.empty()?mesh.vertices.size():mesh.indices.size();
    if(mesh.primitives.empty()) result.source_triangles=available/3;
    else for(const auto& primitive:mesh.primitives) result.source_triangles+=primitive.index_count/3;
    // 仅替换派生级别；原顶点、索引、primitive、名字都保持原样。
    mesh.lods.clear();
    try {
        const auto chain=build_lod_chain(mesh,std::move(options));
        result.attempts=chain.attempts;
        for(std::size_t i=1;i<chain.levels.size();++i) {
            const auto& level=chain.levels[i];
            mesh.lods.push_back({level.mesh.vertices,level.mesh.indices,level.mesh.primitives,level.geometric_error});
        }
        result.stored_levels=mesh.lods.size(); result.reduced=!mesh.lods.empty();
        result.reason=result.reduced?"已保存真实 QEM 减面级别；LOD0 原始几何保持不变":
            "未产生减面级别：边界/材质/UV/法线接缝、拓扑约束或折叠预算阻止简化";
    } catch(const std::invalid_argument& error) {
        // 有界算法的限制是可报告结果；不生成假级别，也不替换源网格。
        result.reason=result.source_triangles>4096?
            "未生成 LOD：真实穷举 QEM 输入预算为 4096 三角形，当前网格超出预算":
            std::string("未生成 LOD：")+error.what();
    }
    if(report) *report=std::move(result);
    return mesh;
}
std::vector<MeshLodBuildReport> build_scene_lods(Scene& scene,
    std::optional<std::size_t> mesh_index,LodBuildOptions options) {
    if(mesh_index && *mesh_index>=scene.meshes.size()) throw std::invalid_argument("LOD mesh index out of range");
    std::vector<MeshLodBuildReport> reports;
    const std::size_t first=mesh_index?*mesh_index:0,last=mesh_index?first+1:scene.meshes.size();
    bool changed=false;
    for(std::size_t i=first;i<last;++i) {
        MeshLodBuildReport report;
        auto built=build_mesh_lods(scene.meshes[i],options,&report); report.mesh=i;
        changed|=!scene.meshes[i].lods.empty() || !built.lods.empty();
        scene.meshes[i].lods=std::move(built.lods);
        reports.push_back(std::move(report));
    }
    // 派生资源发生改变才失效；无实际 reduction 的空结果不伪报成功。
    if(changed) ++scene.revision;
    return reports;
}
double projected_lod_error_pixels(double object_error,const Aabb& object_bounds,const glm::mat4& world,
                                 const Camera& camera,int viewport_height) {
    if(!std::isfinite(object_error) || object_error<0 || viewport_height<=0 ||
       !std::isfinite(camera.fov) || camera.fov<=0 || camera.fov>=180 ||
       !std::isfinite(camera.near_plane) || camera.near_plane<=0 ||
       !finite(camera.position) || !finite(camera.target)) throw std::invalid_argument("Invalid screen-space LOD inputs");
    const auto view_bounds=transform_aabb(object_bounds,camera.view()*world);
    if(object_error==0 || object_bounds.empty()) return 0;
    double norm_one=0,norm_infinity=0;
    for(int i=0;i<3;++i) {
        double column=0,row=0;
        for(int j=0;j<3;++j) { column+=std::abs(double(world[i][j])); row+=std::abs(double(world[j][i])); }
        norm_one=std::max(norm_one,column); norm_infinity=std::max(norm_infinity,row);
    }
    const double error=object_error*std::sqrt(norm_one*norm_infinity);
    const double minimum_depth=-double(view_bounds.max.z)-error;
    if(minimum_depth<=camera.near_plane) return std::numeric_limits<double>::infinity();
    const double x=std::max(std::abs(double(view_bounds.min.x)),std::abs(double(view_bounds.max.x)))+error;
    const double y=std::max(std::abs(double(view_bounds.min.y)),std::abs(double(view_bounds.max.y)))+error;
    const double focal_pixels=viewport_height/(2*std::tan(double(camera.fov)*3.14159265358979323846/360));
    // 透视映射Jacobian的最大奇异值是 f/z*sqrt(1+(x/z)^2+(y/z)^2)。
    // 沿整个位移段采用最坏深度/横向范围，避免近处和视场边缘低估屏幕误差。
    return focal_pixels*error/minimum_depth*std::sqrt(1+(x*x+y*y)/(minimum_depth*minimum_depth));
}
std::size_t select_lod(const LodChain& chain,const glm::mat4& world,const Camera& camera,
                       int viewport_height,double max_error_pixels) {
    if(chain.levels.empty() || !std::isfinite(max_error_pixels) || max_error_pixels<0)
        throw std::invalid_argument("Invalid LOD chain or pixel budget");
    (void)projected_lod_error_pixels(0,chain.bounds,world,camera,viewport_height);
    for(std::size_t i=chain.levels.size();i-->1;)
        if(projected_lod_error_pixels(chain.levels[i].geometric_error,chain.bounds,world,camera,viewport_height)<=max_error_pixels) return i;
    return 0;
}

} // namespace emberframe::lab

