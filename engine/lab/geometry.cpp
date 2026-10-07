#include "geometry.h"
#include "shading.h"
#include <chrono>
#include <cmath>
#include <numeric>
#include <utility>

namespace emberframe::lab {
namespace {
template<glm::length_t N, class T, glm::qualifier Q>
bool finite(const glm::vec<N,T,Q>& v) {
    for (glm::length_t i=0;i<N;++i) if (!std::isfinite(v[i])) return false;
    return true;
}
bool finite_matrix(const glm::mat4& m) {
    for(int i=0;i<4;++i) if(!finite(m[i])) return false;
    return true;
}
void check_affine(const glm::mat4& m) {
    if(!finite_matrix(m) || m[0][3]!=0 || m[1][3]!=0 || m[2][3]!=0 || m[3][3]!=1)
        throw std::invalid_argument("Geometry requires a finite affine transform");
}
bool valid_ray(const Ray& r) {
    return finite(r.origin) && finite(r.direction) &&
        (r.direction.x!=0 || r.direction.y!=0 || r.direction.z!=0) &&
        std::isfinite(r.t_min) && !std::isnan(r.t_max) && r.t_min<=r.t_max;
}
glm::vec3 unit(glm::dvec3 v,glm::vec3 fallback={0,1,0}) {
    const double q=glm::dot(v,v);
    return q>0 && std::isfinite(q) ? glm::vec3(v/std::sqrt(q)) : fallback;
}
glm::vec3 orthogonal(glm::vec3 n) {
    return unit(glm::cross(glm::dvec3(n),std::abs(n.x)<0.8f ? glm::dvec3(1,0,0):glm::dvec3(0,1,0)),{1,0,0});
}
glm::vec4 tangent_frame(glm::vec4 tangent,glm::vec3 normal) {
    const glm::dvec3 t(tangent),n(normal);
    // 浮点单位法线的长度不一定恰好为 1。双叉乘避免平行切线减法残差被归一化成法线。
    const auto projected=glm::cross(n,glm::cross(t,n));
    const auto fallback=orthogonal(normal);
    const auto direction=glm::dot(projected,projected)>glm::dot(t,t)*1e-24 ? unit(projected,fallback):fallback;
    return {direction,tangent.w<0 ? -1.0f:1.0f};
}
bool better(const Hit& candidate,const Hit& current) {
    return candidate.triangle>=0 && (current.triangle<0 || candidate.t<current.t ||
        (candidate.t==current.t && candidate.triangle<current.triangle));
}
Ray shortened(const Ray& ray,const Hit& best) {
    Ray result=ray;
    // Hit 的距离存为 float；向外扩一 ULP，保留舍入到同一距离的编号决胜机会。
    if(best.triangle>=0) result.t_max=std::min(ray.t_max,std::nextafter(best.t,std::numeric_limits<float>::infinity()));
    return result;
}
double milliseconds(std::chrono::steady_clock::time_point start) {
    return std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();
}
void check_triangles(const std::vector<Triangle>& triangles) {
    if(triangles.size()>std::size_t(std::numeric_limits<int>::max()/2))
        throw std::invalid_argument("Too many triangles");
    for(const auto& t:triangles) for(const auto& v:t.vertices)
        if(!finite(v.position)) throw std::invalid_argument("Non-finite triangle position");
}
double plane_distance(const glm::vec4& p,int plane) {
    switch(plane) {
    case 0: return double(p.w)+p.x;
    case 1: return double(p.w)-p.x;
    case 2: return double(p.w)+p.y;
    case 3: return double(p.w)-p.y;
    case 4: return p.z;
    default: return double(p.w)-p.z;
    }
}
bool lex_less(const glm::vec4& a,const glm::vec4& b) {
    for(int i=0;i<4;++i) { if(a[i]<b[i]) return true; if(a[i]>b[i]) return false; }
    return false;
}
template<glm::length_t N>
glm::vec<N,float> interpolate(const glm::vec<N,float>& a,const glm::vec<N,float>& b,double t) {
    return glm::vec<N,float>(glm::vec<N,double>(a)*(1-t)+glm::vec<N,double>(b)*t);
}
ClipVertex clip_intersection(const ClipVertex& first,const ClipVertex& second,int plane) {
    // 共享边反向遍历仍采用相同端点顺序，避免浮点交点产生亚像素裂缝。
    const bool reverse=lex_less(second.clip,first.clip);
    const auto& a=reverse ? second:first;
    const auto& b=reverse ? first:second;
    const double da=plane_distance(a.clip,plane),db=plane_distance(b.clip,plane);
    const double t=da/(da-db);
    ClipVertex c;
    c.clip=interpolate(a.clip,b.clip,t);
    c.attributes.position=interpolate(a.attributes.position,b.attributes.position,t);
    c.attributes.normal=interpolate(a.attributes.normal,b.attributes.normal,t);
    c.attributes.uv=interpolate(a.attributes.uv,b.attributes.uv,t);
    c.attributes.tangent=interpolate(a.attributes.tangent,b.attributes.tangent,t);
    c.attributes.color=interpolate(a.attributes.color,b.attributes.color,t);
    // 烘焙照度与其他顶点属性一样在齐次裁剪边上插值，不丢失新交点照度。
    c.attributes.baked_irradiance=interpolate(a.attributes.baked_irradiance,b.attributes.baked_irradiance,t);
    c.previous_position=interpolate(a.previous_position,b.previous_position,t);
    if(plane<4) c.clip[plane/2]=(plane%2 ? 1.0f:-1.0f)*c.clip.w;
    else c.clip.z=plane==4 ? 0.0f:c.clip.w;
    return c;
}
struct Flattened {
    std::vector<Triangle> triangles;
    std::vector<std::array<glm::vec3,3>> previous;
};
Flattened flatten(const Scene& scene,bool history) {
    const auto worlds=resolve_world_transforms(scene);
    Flattened out;
    auto append=[&](const Mesh& mesh,const glm::mat4& world,const glm::mat4& previous,int object) {
        const glm::dmat3 linear(world);
        const double det=glm::determinant(linear);
        if(!std::isfinite(det) || det==0) throw std::invalid_argument("Singular node transform");
        const glm::dmat3 normals=glm::transpose(glm::inverse(linear));
        if(history) check_affine(previous);
        auto emit=[&](std::size_t first,std::size_t count,int material) {
            const std::size_t available=mesh.indices.empty() ? mesh.vertices.size():mesh.indices.size();
            if(count%3 || first>available || count>available-first)
                throw std::invalid_argument("Invalid triangle-list primitive range");
            for(std::size_t i=first;i<first+count;i+=3) {
                Triangle triangle; triangle.material=material; triangle.object=object;
                std::array<glm::vec3,3> old{};
                for(int k=0;k<3;++k) {
                    const std::size_t index=mesh.indices.empty() ? i+k:mesh.indices[i+k];
                    if(index>=mesh.vertices.size()) throw std::invalid_argument("Mesh index out of range");
                    const Vertex& source=mesh.vertices[index];
                    if(!finite(source.position) || !finite(source.normal) || !finite(source.uv) ||
                       !finite(source.tangent) || !finite(source.color) || !finite(source.baked_irradiance))
                        throw std::invalid_argument("Non-finite vertex attribute");
                    Vertex v=source;
                    v.position=glm::vec3(glm::dmat4(world)*glm::dvec4(source.position,1));
                    // 法线是平面的协向量：非均匀缩放时必须用逆转置，而切线用正向线性变换。
                    v.normal=unit(normals*glm::dvec3(source.normal));
                    const glm::vec3 t=unit(linear*glm::dvec3(source.tangent),orthogonal(v.normal));
                    v.tangent=tangent_frame({t,source.tangent.w*(det<0 ? -1.0f:1.0f)},v.normal);
                    if(!finite(v.position)) throw std::invalid_argument("Transformed position overflow");
                    triangle.vertices[k]=v;
                    if(history) {
                        old[k]=glm::vec3(glm::dmat4(previous)*glm::dvec4(source.position,1));
                        if(!finite(old[k])) throw std::invalid_argument("Previous position overflow");
                    }
                }
                out.triangles.push_back(triangle);
                if(history) out.previous.push_back(old);
            }
        };
        if(mesh.primitives.empty()) emit(0,mesh.indices.empty()?mesh.vertices.size():mesh.indices.size(),0);
        else for(const auto& primitive:mesh.primitives) {
            if(primitive.material>std::uint32_t(std::numeric_limits<int>::max())) throw std::invalid_argument("Material index overflow");
            emit(primitive.first_index,primitive.index_count,int(primitive.material));
        }
    };
    if(scene.nodes.empty()) {
        for(std::size_t i=0;i<scene.meshes.size();++i) append(scene.meshes[i],glm::mat4(1),glm::mat4(1),int(i));
    } else for(std::size_t i=0;i<scene.nodes.size();++i) {
        const auto& node=scene.nodes[i];
        if(node.mesh>=0) append(scene.meshes[std::size_t(node.mesh)],worlds[i],node.previous_world,int(i));
    }
    return out;
}
struct FixedPoint { std::int64_t x,y; };
std::int64_t edge(FixedPoint a,FixedPoint b,FixedPoint p) {
    return (b.x-a.x)*(p.y-a.y)-(b.y-a.y)*(p.x-a.x);
}
bool top_left(FixedPoint a,FixedPoint b) {
    // Y 向下且有向面积为正：向上的边是左边，向右的水平边是顶边。
    return b.y<a.y || (b.y==a.y && b.x>a.x);
}
} // namespace

std::vector<ClippedTriangle> clip_triangle(const ClippedTriangle& triangle) {
    for(const auto& v:triangle)
        if(!finite(v.clip) || !finite(v.attributes.position) || !finite(v.attributes.normal) ||
           !finite(v.attributes.uv) || !finite(v.attributes.tangent) || !finite(v.attributes.color) ||
           !finite(v.attributes.baked_irradiance) ||
           !finite(v.previous_position)) throw std::invalid_argument("Non-finite clip vertex");
    std::vector<ClipVertex> polygon(triangle.begin(),triangle.end()),next;
    for(int plane=0;plane<6 && !polygon.empty();++plane) {
        next.clear();
        auto push=[&](const ClipVertex& v) {
            if(next.empty() || next.back().clip!=v.clip) next.push_back(v);
        };
        ClipVertex previous=polygon.back();
        bool was_inside=plane_distance(previous.clip,plane)>=0;
        for(const auto& current:polygon) {
            const bool inside=plane_distance(current.clip,plane)>=0;
            if(inside!=was_inside) push(clip_intersection(previous,current,plane));
            if(inside) push(current);
            previous=current; was_inside=inside;
        }
        if(next.size()>1 && next.front().clip==next.back().clip) next.pop_back();
        polygon.swap(next);
    }
    std::vector<ClippedTriangle> result;
    for(std::size_t i=1;i+1<polygon.size();++i) result.push_back({polygon[0],polygon[i],polygon[i+1]});
    return result;
}

std::vector<glm::mat4> resolve_world_transforms(const Scene& scene) {
    const std::size_t count=scene.nodes.size();
    std::vector<glm::mat4> world(count,glm::mat4(1));
    std::vector<unsigned char> state(count,0);
    for(const auto& n:scene.nodes) {
        if(n.parent< -1 || (n.parent>=0 && std::size_t(n.parent)>=count) || n.mesh< -1 ||
           (n.mesh>=0 && std::size_t(n.mesh)>=scene.meshes.size())) throw std::invalid_argument("Invalid node reference");
        check_affine(n.local);
    }
    // 显式路径栈既支持父节点晚于子节点，也避免深层场景图耗尽调用栈。
    for(std::size_t start=0;start<count;++start) {
        if(state[start]==2) continue;
        std::vector<std::size_t> path;
        int current=int(start);
        while(current>=0 && state[std::size_t(current)]==0) {
            const auto i=std::size_t(current); state[i]=1; path.push_back(i); current=scene.nodes[i].parent;
        }
        if(current>=0 && state[std::size_t(current)]==1) throw std::invalid_argument("Scene graph cycle");
        for(auto it=path.rbegin();it!=path.rend();++it) {
            const auto i=*it; const auto& node=scene.nodes[i];
            world[i]=node.parent<0 ? node.local:world[std::size_t(node.parent)]*node.local;
            check_affine(world[i]);
            const double det=glm::determinant(glm::dmat3(world[i]));
            if(det==0 || !std::isfinite(det)) throw std::invalid_argument("Singular world transform");
            state[i]=2;
        }
    }
    return world;
}
std::vector<Triangle> flatten_scene(const Scene& scene) { return flatten(scene,false).triangles; }

GBuffer rasterize(const Scene& scene,const Camera& camera,const Settings& settings) {
    return rasterize(scene,camera,settings,RasterOptions{});
}
GBuffer rasterize(const Scene& scene,const Camera& camera,const Settings& settings,const RasterOptions& options) {
    const int width=settings.render_width,height=settings.render_height;
    if(width<=0 || height<=0 || width>(1<<20) || height>(1<<20)) throw std::invalid_argument("Invalid raster dimensions");
    if(!finite(camera.position) || !finite(camera.target) || !std::isfinite(camera.fov) ||
       camera.fov<=0 || camera.fov>=180 || !std::isfinite(camera.near_plane) ||
       !std::isfinite(camera.far_plane)) throw std::invalid_argument("Invalid raster camera");
    const glm::mat4 view=camera.view();
    const glm::mat4 vp=camera.projection(float(width)/float(height),settings.reversed_z)*view;
    if(!finite_matrix(vp)) throw std::invalid_argument("Degenerate camera orientation");
    if(!finite(options.jitter_ndc) || (options.uv_dx && options.uv_dx==options.uv_dy))
        throw std::invalid_argument("Invalid raster options or aliased gradient outputs");
    const auto& previous_camera=options.previous_camera?*options.previous_camera:camera;
    if(!std::isfinite(previous_camera.fov) || previous_camera.fov<=0 || previous_camera.fov>=180 ||
       !std::isfinite(previous_camera.near_plane) || !std::isfinite(previous_camera.far_plane))
        throw std::invalid_argument("Invalid previous camera");
    const glm::mat4 previous_vp=previous_camera.projection(float(width)/float(height),settings.reversed_z)*previous_camera.view();
    if(!finite_matrix(previous_vp)) throw std::invalid_argument("Invalid previous camera orientation");
    SurfaceSample clear; clear.depth=settings.reversed_z?0.0f:1.0f;
    GBuffer buffer(width,height,clear);
    if(options.previous_world_positions) options.previous_world_positions->reset(width,height,glm::vec3(0));
    if(options.uv_dx) options.uv_dx->reset(width,height,glm::vec2(0));
    if(options.uv_dy) options.uv_dy->reset(width,height,glm::vec2(0));
    const auto geometry=flatten(scene,true);
    for(std::size_t ti=0;ti<geometry.triangles.size();++ti) {
        const auto& triangle=geometry.triangles[ti];
        ClippedTriangle input;
        for(int k=0;k<3;++k) {
            input[k]={vp*glm::vec4(triangle.vertices[k].position,1),triangle.vertices[k],geometry.previous[ti][k]};
            input[k].clip.x+=options.jitter_ndc.x*input[k].clip.w;
            input[k].clip.y+=options.jitter_ndc.y*input[k].clip.w;
        }
        for(auto clipped:clip_triangle(input)) {
            std::array<FixedPoint,3> screen{};
            std::array<double,3> inverse_w{},z{};
            bool projectable=true;
            for(int k=0;k<3;++k) {
                if(clipped[k].clip.w<=0) { projectable=false; break; }
                inverse_w[k]=1.0/double(clipped[k].clip.w);
                const double x=(double(clipped[k].clip.x)*inverse_w[k]*0.5+0.5)*width;
                const double y=(0.5-double(clipped[k].clip.y)*inverse_w[k]*0.5)*height;
                screen[k]={std::llround(x*256),std::llround(y*256)};
                z[k]=double(clipped[k].clip.z)*inverse_w[k];
            }
            if(!projectable) continue;
            auto area=edge(screen[0],screen[1],screen[2]);
            if(area==0) continue;
            if(area<0) {
                std::swap(screen[1],screen[2]); std::swap(clipped[1],clipped[2]);
                std::swap(inverse_w[1],inverse_w[2]); std::swap(z[1],z[2]); area=-area;
            }
            const int x0=std::max(0,int(std::min({screen[0].x,screen[1].x,screen[2].x})/256));
            const int x1=std::min(width-1,int(std::max({screen[0].x,screen[1].x,screen[2].x})/256));
            const int y0=std::max(0,int(std::min({screen[0].y,screen[1].y,screen[2].y})/256));
            const int y1=std::min(height-1,int(std::max({screen[0].y,screen[1].y,screen[2].y})/256));
            const bool include0=top_left(screen[1],screen[2]),include1=top_left(screen[2],screen[0]),include2=top_left(screen[0],screen[1]);
            const glm::dvec3 d_lambda_dx=glm::dvec3(screen[1].y-screen[2].y,screen[2].y-screen[0].y,screen[0].y-screen[1].y)*(256.0/double(area));
            const glm::dvec3 d_lambda_dy=glm::dvec3(screen[2].x-screen[1].x,screen[0].x-screen[2].x,screen[1].x-screen[0].x)*(256.0/double(area));
            const glm::dvec3 d_weight_dx=d_lambda_dx*glm::dvec3(inverse_w[0],inverse_w[1],inverse_w[2]);
            const glm::dvec3 d_weight_dy=d_lambda_dy*glm::dvec3(inverse_w[0],inverse_w[1],inverse_w[2]);
            const double d_denominator_dx=d_weight_dx.x+d_weight_dx.y+d_weight_dx.z;
            const double d_denominator_dy=d_weight_dy.x+d_weight_dy.y+d_weight_dy.z;
            glm::dvec2 d_numerator_dx(0),d_numerator_dy(0);
            for(int k=0;k<3;++k) {
                d_numerator_dx+=d_weight_dx[k]*glm::dvec2(clipped[k].attributes.uv);
                d_numerator_dy+=d_weight_dy[k]*glm::dvec2(clipped[k].attributes.uv);
            }
            for(int y=y0;y<=y1;++y) for(int x=x0;x<=x1;++x) {
                const FixedPoint p{std::int64_t(x)*256+128,std::int64_t(y)*256+128};
                const auto e0=edge(screen[1],screen[2],p),e1=edge(screen[2],screen[0],p),e2=edge(screen[0],screen[1],p);
                if(e0<0 || (e0==0 && !include0) || e1<0 || (e1==0 && !include1) || e2<0 || (e2==0 && !include2)) continue;
                const glm::dvec3 lambda{double(e0)/double(area),double(e1)/double(area),double(e2)/double(area)};
                // 硬件深度在屏幕上是 z/w 的仿射插值，不能再除一次插值后的 1/w。
                const float depth=float(std::clamp(lambda.x*z[0]+lambda.y*z[1]+lambda.z*z[2],0.0,1.0));
                auto& target=buffer.at(x,y);
                glm::dvec3 weights=lambda*glm::dvec3(inverse_w[0],inverse_w[1],inverse_w[2]);
                const double divisor=weights.x+weights.y+weights.z;
                if(!(divisor>0) || !std::isfinite(divisor)) continue;
                weights/=divisor;
                SurfaceSample s;
                glm::dvec3 position(0),normal(0),previous(0),baked_irradiance(0);
                glm::dvec2 uv(0); glm::dvec4 tangent(0),color(0);
                // 所有未投影属性共用 (lambda_i/w_i)/sum(lambda_j/w_j)，裁剪交点同样适用。
                for(int k=0;k<3;++k) {
                    const auto& v=clipped[k].attributes;
                    position+=weights[k]*glm::dvec3(v.position); normal+=weights[k]*glm::dvec3(v.normal);
                    uv+=weights[k]*glm::dvec2(v.uv); tangent+=weights[k]*glm::dvec4(v.tangent);
                    color+=weights[k]*glm::dvec4(v.color); previous+=weights[k]*glm::dvec3(clipped[k].previous_position);
                    // 世界空间辐照度使用 reciprocal-w 权重，不能做屏幕仿射插值。
                    baked_irradiance+=weights[k]*glm::dvec3(v.baked_irradiance);
                }
                s.position=glm::vec3(position); s.normal=unit(normal);
                s.uv=glm::vec2(uv); s.tangent=tangent_frame(glm::vec4(tangent),s.normal);
                s.vertex_color=glm::vec4(color); s.depth=depth;
                s.baked_irradiance=glm::vec3(baked_irradiance);
                s.linear_depth=float(-(glm::dmat4(view)*glm::dvec4(position,1)).z);
                s.material=triangle.material; s.object=triangle.object; s.valid=true;
                const auto old_clip=glm::dmat4(previous_vp)*glm::dvec4(previous,1);
                const auto now_clip=glm::dmat4(vp)*glm::dvec4(position,1);
                if(old_clip.w>0 && now_clip.w>0) {
                    // 在同一表面点分别投影，避免定点量化造成静止物体的虚假运动。
                    const glm::dvec2 delta=glm::dvec2(now_clip)/now_clip.w-glm::dvec2(old_clip)/old_clip.w;
                    s.motion=glm::vec2(delta*glm::dvec2(width*0.5,-height*0.5));
                } else s.motion={float(width)*2,float(height)*2}; // 上一帧不可投影，显式越界以拒绝历史。
                // UV=N/D；商法则给每像素的解析导数，不能用仿射UV梯度代替透视足迹。
                const glm::vec2 uv_dx((d_numerator_dx-uv*d_denominator_dx)/divisor);
                const glm::vec2 uv_dy((d_numerator_dy-uv*d_denominator_dy)/divisor);
                s.albedo=glm::vec3(s.vertex_color);
                if(triangle.material>=0 && std::size_t(triangle.material)<scene.materials.size()) {
                    const auto material=sample_material(scene,s,settings,uv_dx,uv_dy);
                    // Alpha-mask必须先丢弃再比较/写深度，镂空纹素不能遮住后面的表面。
                    if(material.alpha_mode==1 && material.base_color.a<material.alpha_cutoff) continue;
                    s.albedo=glm::vec3(material.base_color); s.roughness=material.roughness; s.metallic=material.metallic;
                }
                if(target.valid && (settings.reversed_z ? depth<=target.depth:depth>=target.depth)) continue;
                target=s;
                if(options.previous_world_positions) options.previous_world_positions->at(x,y)=glm::vec3(previous);
                if(options.uv_dx) options.uv_dx->at(x,y)=uv_dx;
                if(options.uv_dy) options.uv_dy->at(x,y)=uv_dy;
            }
        }
    }
    return buffer;
}

bool Aabb::empty() const { return !finite(min) || !finite(max) || min.x>max.x || min.y>max.y || min.z>max.z; }
void Aabb::expand(glm::vec3 point) {
    if(!finite(point)) throw std::invalid_argument("Non-finite bounding point");
    min=glm::min(min,point); max=glm::max(max,point);
}
void Aabb::expand(const Aabb& box) { if(!box.empty()) { expand(box.min); expand(box.max); } }
glm::vec3 Aabb::center() const { return empty()?glm::vec3(0):glm::vec3((glm::dvec3(min)+glm::dvec3(max))*0.5); }
glm::vec3 Aabb::extent() const { return empty()?glm::vec3(0):max-min; }
double Aabb::surface_area() const {
    if(empty()) return 0;
    const glm::dvec3 d=glm::dvec3(max)-glm::dvec3(min);
    return 2*(d.x*d.y+d.y*d.z+d.z*d.x);
}
bool Aabb::contains(glm::vec3 point) const {
    return !empty() && finite(point) && glm::all(glm::greaterThanEqual(point,min)) && glm::all(glm::lessThanEqual(point,max));
}
bool Aabb::contains(const Aabb& box) const { return !box.empty() && contains(box.min) && contains(box.max); }
bool Aabb::overlaps(const Aabb& box) const {
    return !empty() && !box.empty() && glm::all(glm::lessThanEqual(min,box.max)) && glm::all(glm::greaterThanEqual(max,box.min));
}
namespace {
bool slabs(const glm::dvec3& origin,const glm::dvec3& direction,const glm::dvec3& minimum,
           const glm::dvec3& maximum,double& enter,double& exit) {
    for(int axis=0;axis<3;++axis) {
        if(direction[axis]==0) {
            // 显式处理平行轴，避免 0*infinity 产生 NaN，也保留边界上的射线。
            if(origin[axis]<minimum[axis] || origin[axis]>maximum[axis]) return false;
            continue;
        }
        double a=(minimum[axis]-origin[axis])/direction[axis];
        double b=(maximum[axis]-origin[axis])/direction[axis];
        if(a>b) std::swap(a,b);
        // 向外舍入，不让盒测试比精确三角形测试更激进地误剔除擦边命中。
        a=std::nextafter(a,-std::numeric_limits<double>::infinity());
        b=std::nextafter(b,std::numeric_limits<double>::infinity());
        enter=std::max(enter,a); exit=std::min(exit,b);
        if(enter>exit) return false;
    }
    return true;
}
}
bool Aabb::intersect(const Ray& ray,double& enter,double& exit) const {
    if(empty() || !valid_ray(ray)) return false;
    enter=ray.t_min; exit=ray.t_max;
    return slabs(glm::dvec3(ray.origin),glm::dvec3(ray.direction),glm::dvec3(min),glm::dvec3(max),enter,exit);
}
bool Aabb::intersect(const Ray& ray) const { double enter=0,exit=0; return intersect(ray,enter,exit); }
Aabb triangle_bounds(const Triangle& triangle) {
    Aabb box; for(const auto& v:triangle.vertices) box.expand(v.position); return box;
}
Aabb transform_aabb(const Aabb& local,const glm::mat4& transform) {
    check_affine(transform);
    Aabb result;
    if(local.empty()) return result;
    glm::dvec3 minimum(std::numeric_limits<double>::infinity()),maximum(-std::numeric_limits<double>::infinity());
    // 旋转后只变换 min/max 两点会漏角；八个角都参与，再向 float 可表示区间外扩。
    for(int i=0;i<8;++i) {
        const glm::dvec4 p((i&1)?local.max.x:local.min.x,(i&2)?local.max.y:local.min.y,(i&4)?local.max.z:local.min.z,1);
        const glm::dvec3 q(glm::dmat4(transform)*p); minimum=glm::min(minimum,q); maximum=glm::max(maximum,q);
    }
    for(int i=0;i<3;++i) {
        result.min[i]=float(minimum[i]); result.max[i]=float(maximum[i]);
        if(double(result.min[i])>minimum[i]) result.min[i]=std::nextafter(result.min[i],-std::numeric_limits<float>::infinity());
        if(double(result.max[i])<maximum[i]) result.max[i]=std::nextafter(result.max[i],std::numeric_limits<float>::infinity());
    }
    if(result.empty()) throw std::invalid_argument("Transformed bounds overflow");
    return result;
}
Sphere bounding_sphere(const std::vector<Triangle>& triangles) {
    Aabb box; for(const auto& t:triangles) box.expand(triangle_bounds(t));
    Sphere sphere; sphere.center=box.center();
    double r2=0;
    for(const auto& t:triangles) for(const auto& v:t.vertices) {
        const glm::dvec3 d=glm::dvec3(v.position)-glm::dvec3(sphere.center); r2=std::max(r2,glm::dot(d,d));
    }
    const double radius=std::sqrt(r2); sphere.radius=float(radius);
    if(double(sphere.radius)<radius) sphere.radius=std::nextafter(sphere.radius,std::numeric_limits<float>::infinity());
    if(!std::isfinite(sphere.radius)) throw std::invalid_argument("Bounding sphere overflow");
    return sphere;
}
bool intersect_sphere(const Ray& ray,const Sphere& sphere,double& enter,double& exit) {
    if(!valid_ray(ray) || !finite(sphere.center) || !std::isfinite(sphere.radius) || sphere.radius<0) return false;
    const glm::dvec3 d(ray.direction),o=glm::dvec3(ray.origin)-glm::dvec3(sphere.center);
    const double a=glm::dot(d,d),b=glm::dot(o,d),c=glm::dot(o,o)-double(sphere.radius)*sphere.radius;
    const double discriminant=std::fma(b,b,-a*c);
    if(discriminant<0) return false;
    const double q=-b-std::copysign(std::sqrt(discriminant),b);
    double t0=q==0?-b/a:q/a,t1=q==0?t0:c/q;
    if(t0>t1) std::swap(t0,t1);
    enter=std::max(double(ray.t_min),t0); exit=std::min(double(ray.t_max),t1);
    return enter<=exit;
}
bool intersect_obb(const Ray& ray,const Obb& box,double& enter,double& exit) {
    if(!valid_ray(ray) || !finite(box.center) || !finite(box.half_extent) ||
       glm::any(glm::lessThan(box.half_extent,glm::vec3(0)))) return false;
    const glm::dmat3 axes(box.axes);
    for(int i=0;i<3;++i) for(int j=0;j<3;++j)
        if(!finite(axes[i]) || std::abs(glm::dot(axes[i],axes[j])-(i==j?1.0:0.0))>1e-5) return false;
    const auto inv=glm::transpose(axes);
    enter=ray.t_min; exit=ray.t_max;
    return slabs(inv*(glm::dvec3(ray.origin)-glm::dvec3(box.center)),inv*glm::dvec3(ray.direction),
                 -glm::dvec3(box.half_extent),glm::dvec3(box.half_extent),enter,exit);
}
Hit intersect_triangle(const Ray& ray,const Triangle& triangle,int index) {
    Hit miss;
    if(!valid_ray(ray) || index<0) return miss;
    for(const auto& v:triangle.vertices) if(!finite(v.position)) return miss;
    const glm::dvec3 d(ray.direction);
    int kz=0; for(int i=1;i<3;++i) if(std::abs(d[i])>std::abs(d[kz])) kz=i;
    int kx=(kz+1)%3,ky=(kx+1)%3; if(d[kz]<0) std::swap(kx,ky);
    const double sx=-d[kx]/d[kz],sy=-d[ky]/d[kz],sz=1/d[kz];
    std::array<glm::dvec3,3> p;
    // 将射线剪切到 Z 轴，再用共享顶点的二维边函数判断覆盖；不使用尺度相关的 determinant epsilon。
    for(int i=0;i<3;++i) {
        const glm::dvec3 v=glm::dvec3(triangle.vertices[i].position)-glm::dvec3(ray.origin);
        p[i]={v[kx]+sx*v[kz],v[ky]+sy*v[kz],v[kz]*sz};
    }
    const double e0=p[1].x*p[2].y-p[1].y*p[2].x;
    const double e1=p[2].x*p[0].y-p[2].y*p[0].x;
    const double e2=p[0].x*p[1].y-p[0].y*p[1].x;
    if((e0<0 || e1<0 || e2<0) && (e0>0 || e1>0 || e2>0)) return miss;
    const double det=e0+e1+e2;
    if(det==0) return miss;
    const double t=(e0*p[0].z+e1*p[1].z+e2*p[2].z)/det;
    if(!std::isfinite(t) || t<ray.t_min || t>ray.t_max || std::abs(t)>std::numeric_limits<float>::max()) return miss;
    return {float(t),glm::vec3(e0/det,e1/det,e2/det),index};
}
Hit brute_force_intersect(const std::vector<Triangle>& triangles,const Ray& ray,TraversalStatistics* statistics) {
    if(statistics) *statistics={};
    Hit best;
    if(!valid_ray(ray)) return best;
    for(std::size_t i=0;i<triangles.size();++i) {
        if(statistics) ++statistics->triangle_tests;
        const auto h=intersect_triangle(shortened(ray,best),triangles[i],int(i));
        if(better(h,best)) best=h;
    }
    return best;
}

Bvh::Bvh(const std::vector<Triangle>& triangles,BvhBuildOptions options):triangles_(triangles),options_(options) {
    const auto start=std::chrono::steady_clock::now();
    check_triangles(triangles_);
    if(options_.leaf_size==0 || options_.bin_count<2 || options_.bin_count>64 || options_.max_depth>64)
        throw std::invalid_argument("Invalid BVH build options");
    indices_.resize(triangles_.size()); std::iota(indices_.begin(),indices_.end(),0u);
    nodes_.reserve(triangles_.size()*2);
    if(!triangles_.empty()) build(0,std::uint32_t(triangles_.size()),0);
    statistics_.nodes=nodes_.size(); statistics_.primitive_references=indices_.size();
    statistics_.bytes=sizeof(*this)+nodes_.capacity()*sizeof(Node)+indices_.capacity()*sizeof(std::uint32_t)+triangles_.capacity()*sizeof(Triangle);
    statistics_.sah_cost=sah_cost(); statistics_.build_ms=milliseconds(start);
}
std::uint32_t Bvh::build(std::uint32_t first,std::uint32_t count,std::uint32_t depth) {
    const auto node=std::uint32_t(nodes_.size()); nodes_.push_back({});
    Aabb bounds,centroids;
    for(std::uint32_t i=first;i<first+count;++i) {
        const auto b=triangle_bounds(triangles_[indices_[i]]); bounds.expand(b); centroids.expand(b.center());
    }
    nodes_[node].box=bounds;
    statistics_.max_depth=std::max(statistics_.max_depth,depth);
    auto leaf=[&]() { nodes_[node].first=first; nodes_[node].count=count; ++statistics_.leaves; return node; };
    if(count<=options_.leaf_size || depth>=options_.max_depth) return leaf();
    const glm::dvec3 spread=glm::dvec3(centroids.max)-glm::dvec3(centroids.min);
    int axis=0; for(int i=1;i<3;++i) if(spread[i]>spread[axis]) axis=i;
    std::uint32_t middle=first;
    if(options_.split==BvhSplit::binned_sah && bounds.surface_area()>0) {
        struct Bin { Aabb box; std::uint32_t count=0; };
        double best_cost=std::numeric_limits<double>::infinity(); int best_axis=-1,best_split=-1;
        const int bin_count=int(options_.bin_count);
        for(int a=0;a<3;++a) {
            if(spread[a]<=0) continue;
            std::array<Bin,64> bins{};
            for(std::uint32_t i=first;i<first+count;++i) {
                const auto b=triangle_bounds(triangles_[indices_[i]]);
                const int bin=std::clamp(int((double(b.center()[a])-centroids.min[a])/spread[a]*bin_count),0,bin_count-1);
                ++bins[bin].count; bins[bin].box.expand(b);
            }
            std::array<Aabb,64> left_box{},right_box{};
            std::array<std::uint32_t,64> left_count{},right_count{};
            Aabb box; std::uint32_t n=0;
            for(int i=0;i<bin_count;++i) { box.expand(bins[i].box); n+=bins[i].count; left_box[i]=box; left_count[i]=n; }
            box=Aabb{}; n=0;
            for(int i=bin_count-1;i>=0;--i) { box.expand(bins[i].box); n+=bins[i].count; right_box[i]=box; right_count[i]=n; }
            for(int i=0;i<bin_count-1;++i) {
                if(left_count[i]==0 || right_count[i+1]==0) continue;
                // SAH: 一次盒遍历 + 两侧面积概率乘各自图元代价；这里盒/三角形代价都取 1。
                const double cost=1+(left_box[i].surface_area()*left_count[i]+right_box[i+1].surface_area()*right_count[i+1])/bounds.surface_area();
                if(cost<best_cost) { best_cost=cost; best_axis=a; best_split=i; }
            }
        }
        if(best_axis>=0) {
            if(best_cost>=count) return leaf();
            const auto it=std::stable_partition(indices_.begin()+first,indices_.begin()+first+count,[&](std::uint32_t id) {
                const auto center=triangle_bounds(triangles_[id]).center();
                const int bin=std::clamp(int((double(center[best_axis])-centroids.min[best_axis])/spread[best_axis]*bin_count),0,bin_count-1);
                return bin<=best_split;
            });
            middle=std::uint32_t(it-indices_.begin());
        }
    }
    if(middle==first || middle==first+count) {
        // 质心重合时仍按原编号作稳定二分，保证终止且结果可重复。
        std::stable_sort(indices_.begin()+first,indices_.begin()+first+count,[&](std::uint32_t a,std::uint32_t b) {
            const float ca=triangle_bounds(triangles_[a]).center()[axis],cb=triangle_bounds(triangles_[b]).center()[axis];
            return ca==cb ? a<b:ca<cb;
        });
        middle=first+count/2;
    }
    const auto left=build(first,middle-first,depth+1),right=build(middle,first+count-middle,depth+1);
    nodes_[node].left=left; nodes_[node].right=right;
    return node;
}
double Bvh::sah_cost() const {
    if(nodes_.empty()) return 0;
    std::vector<double> cost(nodes_.size(),0);
    for(std::size_t i=nodes_.size();i-->0;) {
        const auto& n=nodes_[i];
        if(n.count) cost[i]=n.count;
        else {
            const double area=n.box.surface_area();
            cost[i]=1+(area>0 ? (nodes_[n.left].box.surface_area()*cost[n.left]+nodes_[n.right].box.surface_area()*cost[n.right])/area : cost[n.left]+cost[n.right]);
        }
    }
    return cost[0];
}
Aabb Bvh::bounds() const { return nodes_.empty()?Aabb{}:nodes_[0].box; }
Hit Bvh::traverse(const Ray& ray,bool any,TraversalStatistics* statistics) const {
    if(statistics) *statistics={};
    Hit best;
    if(nodes_.empty() || !valid_ray(ray)) return best;
    struct Entry { std::uint32_t node; double enter; };
    std::vector<Entry> stack;
    double enter=0,exit=0;
    if(statistics) ++statistics->box_tests;
    if(!nodes_[0].box.intersect(ray,enter,exit)) return best;
    stack.push_back({0,enter});
    while(!stack.empty()) {
        const auto entry=stack.back(); stack.pop_back();
        const Ray clipped=shortened(ray,best);
        if(entry.enter>clipped.t_max) continue;
        const auto& node=nodes_[entry.node];
        if(statistics) ++statistics->nodes_visited;
        if(node.count) {
            for(std::uint32_t i=node.first;i<node.first+node.count;++i) {
                const auto id=indices_[i];
                if(statistics) ++statistics->triangle_tests;
                const auto hit=intersect_triangle(shortened(ray,best),triangles_[id],int(id));
                if(better(hit,best)) { best=hit; if(any) return best; }
            }
        } else {
            double left_enter=0,right_enter=0;
            if(statistics) statistics->box_tests+=2;
            const bool left=nodes_[node.left].box.intersect(clipped,left_enter,exit);
            const bool right=nodes_[node.right].box.intersect(clipped,right_enter,exit);
            if(left && right) {
                if(left_enter<=right_enter) { stack.push_back({node.right,right_enter}); stack.push_back({node.left,left_enter}); }
                else { stack.push_back({node.left,left_enter}); stack.push_back({node.right,right_enter}); }
            } else if(left) stack.push_back({node.left,left_enter});
            else if(right) stack.push_back({node.right,right_enter});
        }
    }
    return best;
}
Hit Bvh::intersect(const Ray& ray,TraversalStatistics* statistics) const { return traverse(ray,false,statistics); }
bool Bvh::occluded(const Ray& ray,TraversalStatistics* statistics) const { return traverse(ray,true,statistics).triangle>=0; }
void Bvh::refit(const std::vector<Triangle>& triangles) {
    if(triangles.size()!=triangles_.size()) throw std::invalid_argument("BVH refit requires unchanged triangle count");
    check_triangles(triangles);
    const auto start=std::chrono::steady_clock::now(); triangles_=triangles;
    for(std::size_t i=nodes_.size();i-->0;) {
        auto& node=nodes_[i]; node.box=Aabb{};
        if(node.count) for(std::uint32_t j=node.first;j<node.first+node.count;++j) node.box.expand(triangle_bounds(triangles_[indices_[j]]));
        else { node.box.expand(nodes_[node.left].box); node.box.expand(nodes_[node.right].box); }
    }
    statistics_.refit_nodes=nodes_.size(); statistics_.sah_cost=sah_cost(); statistics_.refit_ms=milliseconds(start);
}

Octree::Octree(const std::vector<Triangle>& triangles,OctreeOptions options):options_(options) {
    if(options_.leaf_size==0 || options_.max_depth>32) throw std::invalid_argument("Invalid octree options");
    rebuild(triangles);
}
void Octree::rebuild(const std::vector<Triangle>& triangles) {
    check_triangles(triangles);
    const auto start=std::chrono::steady_clock::now();
    triangles_=triangles; nodes_.clear(); statistics_={};
    Aabb root; for(const auto& t:triangles_) root.expand(triangle_bounds(t));
    std::vector<std::uint32_t> ids(triangles_.size()); std::iota(ids.begin(),ids.end(),0u);
    if(!ids.empty()) build(root,std::move(ids),0);
    statistics_.nodes=nodes_.size(); statistics_.primitive_references=triangles_.size();
    statistics_.bytes=sizeof(*this)+nodes_.capacity()*sizeof(Node)+triangles_.capacity()*sizeof(Triangle);
    for(const auto& node:nodes_) statistics_.bytes+=node.resident.capacity()*sizeof(std::uint32_t);
    statistics_.build_ms=milliseconds(start);
}
int Octree::build(const Aabb& box,std::vector<std::uint32_t> ids,std::uint32_t depth) {
    const int node=int(nodes_.size()); nodes_.push_back({}); nodes_[std::size_t(node)].box=box;
    statistics_.max_depth=std::max(statistics_.max_depth,depth);
    auto leaf=[&]() { nodes_[std::size_t(node)].resident=std::move(ids); ++statistics_.leaves; return node; };
    if(ids.size()<=options_.leaf_size || depth>=options_.max_depth) return leaf();
    const glm::vec3 midpoint=box.center();
    std::array<std::vector<std::uint32_t>,8> groups;
    std::vector<std::uint32_t> residents;
    for(const auto id:ids) {
        const auto bounds=triangle_bounds(triangles_[id]);
        int child=0; bool fits=true;
        for(int axis=0;axis<3;++axis) {
            if(bounds.max[axis]<=midpoint[axis]) continue;
            if(bounds.min[axis]>=midpoint[axis]) child|=1<<axis;
            else { fits=false; break; }
        }
        // 跨分割面的三角形留在父节点，引用只存一份；查询父节点时必须先检查它们。
        if(fits) groups[std::size_t(child)].push_back(id); else residents.push_back(id);
    }
    bool has_children=false;
    for(int child=0;child<8;++child) {
        auto& group=groups[std::size_t(child)];
        if(group.empty()) continue;
        Aabb child_box=box;
        for(int axis=0;axis<3;++axis) {
            if(child&(1<<axis)) child_box.min[axis]=midpoint[axis]; else child_box.max[axis]=midpoint[axis];
        }
        if(child_box.min==box.min && child_box.max==box.max) {
            residents.insert(residents.end(),group.begin(),group.end()); continue;
        }
        const int id=build(child_box,std::move(group),depth+1);
        nodes_[std::size_t(node)].children[std::size_t(child)]=id; has_children=true;
    }
    nodes_[std::size_t(node)].resident=std::move(residents);
    if(!has_children) ++statistics_.leaves;
    return node;
}
Aabb Octree::bounds() const { return nodes_.empty()?Aabb{}:nodes_[0].box; }
Hit Octree::traverse(const Ray& ray,bool any,TraversalStatistics* statistics) const {
    if(statistics) *statistics={};
    Hit best;
    if(nodes_.empty() || !valid_ray(ray)) return best;
    struct Entry { int node; double enter; };
    std::vector<Entry> stack;
    double enter=0,exit=0;
    if(statistics) ++statistics->box_tests;
    if(!nodes_[0].box.intersect(ray,enter,exit)) return best;
    stack.push_back({0,enter});
    while(!stack.empty()) {
        const auto entry=stack.back(); stack.pop_back();
        if(entry.enter>shortened(ray,best).t_max) continue;
        const auto& node=nodes_[std::size_t(entry.node)];
        if(statistics) ++statistics->nodes_visited;
        for(const auto id:node.resident) {
            if(statistics) ++statistics->triangle_tests;
            const auto hit=intersect_triangle(shortened(ray,best),triangles_[id],int(id));
            if(better(hit,best)) { best=hit; if(any) return best; }
        }
        std::array<Entry,8> children{}; std::size_t count=0;
        for(const int child:node.children) if(child>=0) {
            if(statistics) ++statistics->box_tests;
            if(nodes_[std::size_t(child)].box.intersect(shortened(ray,best),enter,exit)) children[count++]={child,enter};
        }
        // 子节点至多八个；直接插入排序避免通用排序的额外开销。
        for(std::size_t i=1;i<count;++i) {
            const auto value=children[i]; std::size_t j=i;
            while(j>0 && (children[j-1].enter<value.enter ||
                  (children[j-1].enter==value.enter && children[j-1].node<value.node))) {
                children[j]=children[j-1]; --j;
            }
            children[j]=value;
        }
        for(std::size_t i=0;i<count;++i) stack.push_back(children[i]);
    }
    return best;
}
Hit Octree::intersect(const Ray& ray,TraversalStatistics* statistics) const { return traverse(ray,false,statistics); }
bool Octree::occluded(const Ray& ray,TraversalStatistics* statistics) const { return traverse(ray,true,statistics).triangle>=0; }
std::vector<std::uint32_t> Octree::query(const Aabb& region,TraversalStatistics* statistics) const {
    if(statistics) *statistics={};
    std::vector<std::uint32_t> result;
    if(nodes_.empty() || region.empty()) return result;
    std::vector<int> stack{0};
    while(!stack.empty()) {
        const int id=stack.back(); stack.pop_back(); const auto& node=nodes_[std::size_t(id)];
        if(statistics) ++statistics->box_tests;
        if(!node.box.overlaps(region)) continue;
        if(statistics) ++statistics->nodes_visited;
        for(const auto triangle:node.resident) {
            if(statistics) ++statistics->triangle_tests;
            if(triangle_bounds(triangles_[triangle]).overlaps(region)) result.push_back(triangle);
        }
        for(const int child:node.children) if(child>=0) stack.push_back(child);
    }
    std::sort(result.begin(),result.end());
    return result;
}


} // namespace emberframe::lab
