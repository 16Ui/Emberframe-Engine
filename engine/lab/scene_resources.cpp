#include "scene_resources.h"
#include "geometry.h"
#include "shading.h"
#include <bit>
#include <mutex>
#include <numeric>
#include <string_view>

namespace emberframe::lab {
namespace {
// 有界 CPU 参考预算：不降分辨率、不伪造成功，调用者可降低设置后重试。
constexpr std::uint64_t max_bytes=64ull*1024*1024;
constexpr std::uint64_t max_cache_bytes=128ull*1024*1024;
constexpr std::uint64_t max_vertices=500000,max_triangles=250000,max_instances=65536;
constexpr std::uint64_t max_prt_rays=8000000,max_distance_tests=16000000,max_node_tests=64000000;
constexpr std::uint64_t hash_seed=14695981039346656037ull;
bool cancelled(const std::atomic<bool>* c) { return c&&c->load(std::memory_order_relaxed); }
bool finite(glm::vec3 v) { return std::isfinite(v.x)&&std::isfinite(v.y)&&std::isfinite(v.z); }
struct BakeCancelled {};
void check_cancel(std::atomic<bool>* c) { if(cancelled(c))throw BakeCancelled{}; }
struct Hash {
    std::uint64_t value=hash_seed;
    void byte(unsigned char x) { value=(value^x)*1099511628211ull; }
    void u64(std::uint64_t x) { for(int i=0;i<8;++i)byte(static_cast<unsigned char>(x>>(8*i))); }
    void scalar(float x) {
        if(!std::isfinite(x))throw std::invalid_argument("Non-finite bake geometry/environment");
        // +0 与 -0 的几何意义相同，且不散列对象内存中的填充字节。
        u64(std::bit_cast<std::uint32_t>(x==0?0.f:x));
    }
    void vector(glm::vec3 v) { for(int i=0;i<3;++i)scalar(v[i]); }
};
std::uint64_t bytes_hash(std::string_view bytes) {
    Hash h;for(unsigned char c:bytes)h.byte(c);return h.value;
}
std::uint64_t environment_hash(const Scene& scene) {
    Hash h;h.vector(scene.sky_top);h.vector(scene.sky_bottom);return h.value;
}
EnvironmentSh9 project_environment(const Scene& scene) {
    EnvironmentSh9 result{};
    for(int channel=0;channel<3;++channel) {
        // 当前真实天空是 max(a+b*y,0)。对每个通道解析积分，常量/线性天空没有采样噪声。
        const double a=(double(scene.sky_top[channel])+scene.sky_bottom[channel])*.5;
        const double b=(double(scene.sky_top[channel])-scene.sky_bottom[channel])*.5;
        double lo=-1,hi=1;
        if(b>0)lo=std::clamp(-a/b,-1.,1.);
        else if(b<0)hi=std::clamp(-a/b,-1.,1.);
        else if(a<=0)continue;
        auto integral=[&](int power) {
            return a*(std::pow(hi,power+1)-std::pow(lo,power+1))/(power+1)+
                   b*(std::pow(hi,power+2)-std::pow(lo,power+2))/(power+2);
        };
        const double m0=integral(0),m1=integral(1),m2=integral(2),tau=2*double(pi);
        result[0][channel]=float(tau*.2820947918*m0);
        result[1][channel]=float(tau*.4886025119*m1);
        // 绕 Y 对称时其他系数为零；裁零负辐亮度仍可产生真实的二阶项。
        result[6][channel]=float(tau*.3153915653*(m0*.5-m2*1.5));
        result[8][channel]=float(tau*.5462742153*(m0*.5-m2*1.5));
    }
    return result;
}
struct Instance { int node,mesh;glm::mat4 world; };
std::vector<Instance> instances(const Scene& scene) {
    if(scene.nodes.size()>max_instances||scene.meshes.size()>max_instances)
        throw std::length_error("Scene bake instance budget exceeded");
    const auto worlds=resolve_world_transforms(scene);
    std::vector<Instance> result;
    if(scene.nodes.empty()) {
        for(std::size_t i=0;i<scene.meshes.size();++i)result.push_back({-1,int(i),glm::mat4(1)});
    }else for(std::size_t i=0;i<scene.nodes.size();++i)
        if(scene.nodes[i].mesh>=0)result.push_back({int(i),scene.nodes[i].mesh,worlds[i]});
    return result;
}
std::uint64_t vertex_count(const Scene& scene,const std::vector<Instance>& list) {
    std::uint64_t count=0;
    for(const auto& instance:list) {
        count+=scene.meshes[std::size_t(instance.mesh)].vertices.size();
        if(count>max_vertices)throw std::length_error("Scene bake vertex budget exceeded");
    }
    return count;
}
std::uint64_t triangle_count(const Scene& scene,const std::vector<Instance>& list) {
    std::uint64_t total=0;
    for(const auto& instance:list) {
        const auto& mesh=scene.meshes[std::size_t(instance.mesh)];
        const auto available=mesh.indices.empty()?mesh.vertices.size():mesh.indices.size();
        auto range=[&](std::uint64_t first,std::uint64_t count) {
            if(first>available||count>available-first||count%3)
                throw std::invalid_argument("Invalid scene bake triangle range");
            total+=count/3;
            if(total>max_triangles)throw std::length_error("Scene bake triangle budget exceeded");
        };
        if(mesh.primitives.empty())range(0,available);
        else for(const auto& primitive:mesh.primitives)range(primitive.first_index,primitive.index_count);
    }
    return total;
}
std::uint64_t resource_bytes(const SceneBakeResources& r) {
    std::uint64_t bytes=sizeof(r);
    if(r.prt)for(const auto& i:r.prt->instances)bytes+=sizeof(i)+i.transfer.size()*sizeof(Sh9);
    if(r.sdf)bytes+=sizeof(SceneSdfGrid)+r.sdf->values.size()*sizeof(float);
    return bytes;
}
void validate_resource(const SceneBakeResources& r) {
    if(r.bake_samples<16||r.bake_samples>4096)throw std::invalid_argument("Invalid resource sample count");
    for(auto c:r.environment_sh9)if(!finite(c))throw std::invalid_argument("Non-finite environment SH");
    if(r.prt) {
        if(r.prt->instances.size()>max_instances)throw std::invalid_argument("Invalid PRT instance count");
        std::uint64_t vertices=0;int previous_node=-2,previous_mesh=-1;
        for(const auto& instance:r.prt->instances) {
            if(instance.node< -1||instance.node>=int(max_instances)||instance.mesh<0||instance.mesh>=int(max_instances))
                throw std::invalid_argument("Invalid PRT instance reference");
            if(instance.node==-1) {
                if(previous_node>=0||instance.mesh<=previous_mesh)throw std::invalid_argument("Invalid PRT mesh ordering");
            }else if(instance.node<=previous_node)throw std::invalid_argument("Invalid PRT node ordering");
            previous_node=instance.node;previous_mesh=instance.mesh;
            vertices+=instance.transfer.size();
            if(vertices>max_vertices)throw std::invalid_argument("Invalid PRT vertex count");
            for(const auto& transfer:instance.transfer)for(float x:transfer)
                // integral |Y*cos| <= pi*max|Y|，留出浮点舍入范围。
                if(!std::isfinite(x)||std::abs(x)>4)throw std::invalid_argument("Invalid PRT coefficient");
        }
        if(vertices*std::uint64_t(r.bake_samples)>max_prt_rays)
            throw std::invalid_argument("PRT ray budget exceeded");
    }
    if(r.sdf) {
        const auto& g=*r.sdf;
        if(!g.unsigned_distance||g.resolution<4||g.resolution>64||!finite(g.min)||!finite(g.max)||
           glm::any(glm::lessThanEqual(g.max,g.min))||!finite(g.max-g.min)||
           g.values.size()!=std::uint64_t(g.resolution)*g.resolution*g.resolution)
            throw std::invalid_argument("Invalid unsigned scene SDF layout");
        const float limit=glm::length(g.max-g.min)*1.01f;
        if(!std::isfinite(limit))throw std::invalid_argument("Invalid scene SDF extent");
        for(float x:g.values)if(!std::isfinite(x)||x<0||x>limit)
            throw std::invalid_argument("Invalid unsigned scene SDF value");
    }
    if(resource_bytes(r)>max_bytes)throw std::invalid_argument("Scene resource byte budget exceeded");
}
bool satisfies(const SceneBakeResources& r,std::uint64_t geometry,const Settings& s) {
    return r.geometry_hash==geometry&&
        (s.environment_diffuse!=EnvironmentDiffuse::prt||(r.prt&&r.bake_samples==s.bake_samples))&&
        (!s.sdf_shadows||(r.sdf&&r.sdf->resolution==s.sdf_resolution));
}
// 点到真实三角面的最近距离；退化三角形退化为三条线段，不产生 NaN。
struct TriangleNearest { double squared;glm::dvec3 barycentric; };
TriangleNearest triangle_nearest(glm::dvec3 p,const Triangle& triangle) {
    const glm::dvec3 a(triangle.vertices[0].position),b(triangle.vertices[1].position),c(triangle.vertices[2].position);
    const auto ab=b-a,ac=c-a,n=glm::cross(ab,ac);const double n2=glm::dot(n,n);
    if(n2>0) {
        const auto projected=p-n*(glm::dot(p-a,n)/n2);
        const auto q=projected-a;
        const double d00=glm::dot(ab,ab),d01=glm::dot(ab,ac),d11=glm::dot(ac,ac);
        const double d20=glm::dot(q,ab),d21=glm::dot(q,ac),denominator=d00*d11-d01*d01;
        if(denominator>0) {
            const double v=(d11*d20-d01*d21)/denominator,w=(d00*d21-d01*d20)/denominator;
            if(v>=0&&w>=0&&v+w<=1) {
                const auto d=p-projected;return {glm::dot(d,d),{1-v-w,v,w}};
            }
        }
    }
    TriangleNearest best{std::numeric_limits<double>::infinity(),{1,0,0}};
    const std::array<glm::dvec3,3> points{a,b,c};
    for(int i=0;i<3;++i) {
        const int j=(i+1)%3;const auto e=points[j]-points[i];const double length=glm::dot(e,e);
        const double t=length>0?std::clamp(glm::dot(p-points[i],e)/length,0.,1.):0.;
        const auto d=p-(points[i]+e*t);const double q=glm::dot(d,d);
        if(q<best.squared) { best.squared=q;best.barycentric=glm::dvec3(0);best.barycentric[i]=1-t;best.barycentric[j]=t; }
    }
    return best;
}
struct DistanceBudget { std::uint64_t triangles=0,nodes=0; };
// 最近表面距离专用 BVH，按包围盒距离剪枝；原有射线 BVH 的节点保持封装。
class DistanceBvh {
    struct Node { Aabb bounds;std::uint32_t first=0,count=0,left=0,right=0; };
    const std::vector<Triangle>& triangles_;
    std::vector<Aabb> boxes_;
    std::vector<std::uint32_t> indices_;
    std::vector<Node> nodes_;
    std::atomic<bool>* cancel_;
    std::uint32_t build(std::uint32_t first,std::uint32_t count) {
        check_cancel(cancel_);const auto id=std::uint32_t(nodes_.size());nodes_.push_back({});
        Aabb box;for(std::uint32_t i=first;i<first+count;++i)box.expand(boxes_[indices_[i]]);
        nodes_[id].bounds=box;
        if(count<=4) { nodes_[id].first=first;nodes_[id].count=count;return id; }
        const auto e=box.extent();const int axis=e.x>=e.y&&e.x>=e.z?0:e.y>=e.z?1:2;
        const auto middle=first+count/2;
        std::nth_element(indices_.begin()+first,indices_.begin()+middle,indices_.begin()+first+count,
            [&](std::uint32_t a,std::uint32_t b){return boxes_[a].center()[axis]<boxes_[b].center()[axis];});
        const auto left=build(first,count/2),right=build(middle,count-count/2);
        nodes_[id].left=left;nodes_[id].right=right;return id;
    }
    static double squared(const Aabb& box,glm::dvec3 p) {
        const auto delta=p-glm::clamp(p,glm::dvec3(box.min),glm::dvec3(box.max));return glm::dot(delta,delta);
    }
public:
    struct Result { double squared=std::numeric_limits<double>::infinity();int triangle=-1;glm::dvec3 barycentric{1,0,0}; };
    explicit DistanceBvh(const std::vector<Triangle>& triangles,std::atomic<bool>* cancel):triangles_(triangles),cancel_(cancel) {
        boxes_.reserve(triangles.size());indices_.resize(triangles.size());std::iota(indices_.begin(),indices_.end(),0u);
        for(const auto& t:triangles) { check_cancel(cancel);boxes_.push_back(triangle_bounds(t)); }
        if(!triangles.empty())build(0,std::uint32_t(triangles.size()));
    }
    Result nearest(glm::dvec3 point,DistanceBudget& budget) const {
        Result best;if(nodes_.empty())return best;
        // 中位划分深度最多 18（<=250k 三角形），栈容量 64 足够且没有每格点动态分配。
        struct Entry { std::uint32_t id;double distance; };
        std::array<Entry,64> stack{};std::size_t size=1;stack[0]={0,0};
        while(size) {
            check_cancel(cancel_);const auto entry=stack[--size];
            if(entry.distance>best.squared)continue;
            if(++budget.nodes>max_node_tests)throw std::length_error("SDF/LOD nearest-distance node budget exceeded");
            const auto& node=nodes_[entry.id];
            if(node.count)for(std::uint32_t i=node.first;i<node.first+node.count;++i) {
                if(++budget.triangles>max_distance_tests)throw std::length_error("SDF/LOD exceeds 16M exact-distance test budget");
                const auto id=indices_[i];const auto result=triangle_nearest(point,triangles_[id]);
                if(result.squared<best.squared||(result.squared==best.squared&&(best.triangle<0||int(id)<best.triangle)))
                    best={result.squared,int(id),result.barycentric};
            }else {
                const double a=squared(nodes_[node.left].bounds,point),b=squared(nodes_[node.right].bounds,point);
                if(a<b) { stack[size++]={node.right,b};stack[size++]={node.left,a}; }
                else { stack[size++]={node.left,a};stack[size++]={node.right,b}; }
            }
        }
        return best;
    }
};
std::shared_ptr<const ScenePrtResources> bake_prt(const Scene& scene,const std::vector<Instance>& list,
                                               const Bvh& bvh,int samples,std::atomic<bool>* cancel) {
    auto data=std::make_shared<ScenePrtResources>();
    const float extent=bvh.bounds().empty()?1.f:glm::length(bvh.bounds().extent());
    const float offset=std::max(1e-5f,extent*1e-5f);
    std::uint64_t traversal_work=0;
    for(const auto& instance:list) {
        ScenePrtInstance output;output.node=instance.node;output.mesh=instance.mesh;
        const auto& mesh=scene.meshes[std::size_t(instance.mesh)];output.transfer.resize(mesh.vertices.size());
        const glm::dmat3 normals=glm::transpose(glm::inverse(glm::dmat3(instance.world)));
        for(std::size_t i=0;i<mesh.vertices.size();++i) {
            if(cancelled(cancel))return {};
            const auto& vertex=mesh.vertices[i];
            const auto position=glm::vec3(glm::dmat4(instance.world)*glm::dvec4(vertex.position,1));
            const auto transformed=normals*glm::dvec3(vertex.normal);
            const double length=glm::length(transformed);
            if(!finite(position)||!std::isfinite(length)||length==0)
                throw std::invalid_argument("Invalid PRT position/normal");
            const auto normal=glm::vec3(transformed/length);
            // 使用既有 PRT 的余弦采样/SH 顺序，取消在可见性回调中及时中断。
            const auto transfer=bake_diffuse_prt(position,normal,[&](glm::vec3 origin,glm::vec3 direction) {
                check_cancel(cancel);TraversalStatistics stats;
                const bool blocked=bvh.occluded({origin,direction,offset*.1f,1e30f},&stats);
                traversal_work+=stats.box_tests+stats.triangle_tests;
                if(traversal_work>max_node_tests)throw std::length_error("PRT BVH traversal budget exceeded");
                return blocked?0.f:1.f;
            },{samples,offset});
            for(int k=0;k<9;++k)output.transfer[i][k]=transfer.transfer[k].x;
        }
        data->instances.push_back(std::move(output));
    }
    return data;
}
std::shared_ptr<const SceneSdfGrid> bake_sdf(const std::vector<Triangle>& triangles,int resolution,
                                         std::atomic<bool>* cancel) {
    if(triangles.empty())return {};
    auto grid=std::make_shared<SceneSdfGrid>();grid->resolution=resolution;
    Aabb box;for(const auto& t:triangles)box.expand(triangle_bounds(t));
    const auto extent=box.extent();const float longest=std::max({extent.x,extent.y,extent.z,1e-3f});
    // 各轴至少有长度，平面/开放 mesh 同样可烘焙；边界外没有三角面。
    const float padding=longest/float(resolution-1)*2;
    grid->min=box.min-glm::vec3(padding);grid->max=box.max+glm::vec3(padding);
    if(!finite(grid->min)||!finite(grid->max)||!std::isfinite(glm::length(grid->max-grid->min)))
        throw std::invalid_argument("SDF bounds overflow");
    grid->values.resize(std::size_t(resolution)*resolution*resolution);
    const DistanceBvh nearest(triangles,cancel);DistanceBudget budget;
    const glm::dvec3 step=glm::dvec3(grid->max-grid->min)/double(resolution-1);
    for(int z=0;z<resolution;++z)for(int y=0;y<resolution;++y)for(int x=0;x<resolution;++x) {
        if(cancelled(cancel))return {};
        const auto p=glm::dvec3(grid->min)+step*glm::dvec3(x,y,z);
        grid->values[(std::size_t(z)*resolution+y)*resolution+x]=float(std::sqrt(nearest.nearest(p,budget).squared));
    }
    return grid;
}
struct Writer {
    std::string data;
    void u32(std::uint32_t x) { for(int i=0;i<4;++i)data.push_back(char(x>>(8*i))); }
    void u64(std::uint64_t x) { for(int i=0;i<8;++i)data.push_back(char(x>>(8*i))); }
    void scalar(float x) { u32(std::bit_cast<std::uint32_t>(x)); }
    void vector(glm::vec3 v) { for(int i=0;i<3;++i)scalar(v[i]); }
};
struct Reader {
    std::string_view data;std::size_t offset=0;
    std::uint64_t read(int bytes) {
        if(std::size_t(bytes)>data.size()-offset)throw std::invalid_argument("Truncated scene resource");
        std::uint64_t x=0;for(int i=0;i<bytes;++i)x|=std::uint64_t(static_cast<unsigned char>(data[offset++]))<<(8*i);return x;
    }
    std::uint32_t u32() { return std::uint32_t(read(4)); }
    std::uint64_t u64() { return read(8); }
    float scalar() { return std::bit_cast<float>(u32()); }
    glm::vec3 vector() { glm::vec3 v;for(int i=0;i<3;++i)v[i]=scalar();return v; }
};
} // namespace

std::uint64_t geometry_fingerprint(const Scene& scene) {
    const auto list=instances(scene);(void)vertex_count(scene,list);(void)triangle_count(scene,list);
    Hash h;h.u64(1);h.u64(list.size());
    // 散列实例实际数据而非 mesh 编号：apply 的实例副本不改变指纹。
    for(const auto& instance:list) {
        h.u64(std::uint64_t(instance.node+1));
        for(int c=0;c<4;++c)for(int r=0;r<4;++r)h.scalar(instance.world[c][r]);
        const auto& mesh=scene.meshes[std::size_t(instance.mesh)];h.u64(mesh.vertices.size());
        for(const auto& v:mesh.vertices) { h.vector(v.position);h.vector(v.normal); }
        h.u64(mesh.indices.size());for(auto index:mesh.indices) {
            if(index>=mesh.vertices.size())throw std::invalid_argument("Scene bake index out of range");
            h.u64(index);
        }
        h.u64(mesh.primitives.size());
        for(const auto& primitive:mesh.primitives) { h.u64(primitive.first_index);h.u64(primitive.index_count); }
    }
    return h.value;
}
std::shared_ptr<const SceneBakeResources> prepare_scene_resources(const Scene& scene,const Settings& s,
                                                               std::atomic<bool>* cancel) {
    if(cancelled(cancel))return {};
    if(s.bake_samples<16||s.bake_samples>4096||s.sdf_resolution<4||s.sdf_resolution>64||
       int(s.environment_diffuse)<0||int(s.environment_diffuse)>2)
        throw std::invalid_argument("Scene bake samples [16,4096], resolution [4,64] required");
    const auto geometry=geometry_fingerprint(scene),env=environment_hash(scene);
    if(cancelled(cancel))return {};
    static std::mutex mutex;
    static std::vector<std::shared_ptr<const SceneBakeResources>> cache;
    // 同键并发创建串行化，已公布快照始终只读，避免重复昂贵烘焙。
    std::lock_guard lock(mutex);
    if(cancelled(cancel))return {};
    auto complete=[&](const std::shared_ptr<const SceneBakeResources>& r) {
        return r&&r->environment_hash==env&&satisfies(*r,geometry,s);
    };
    if(complete(scene.baked_resources)) { validate_resource(*scene.baked_resources);return scene.baked_resources; }
    for(const auto& r:cache)if(complete(r))return r;
    auto result=std::make_shared<SceneBakeResources>();result->geometry_hash=geometry;
    result->environment_hash=env;result->bake_samples=s.bake_samples;
    auto reuse=[&](const std::shared_ptr<const SceneBakeResources>& r) {
        if(!r||r->geometry_hash!=geometry)return;
        if(!result->prt&&r->prt&&r->bake_samples==s.bake_samples)result->prt=r->prt;
        if(!result->sdf&&r->sdf&&r->sdf->resolution==s.sdf_resolution)result->sdf=r->sdf;
    };
    if(scene.baked_resources&&scene.baked_resources->geometry_hash==geometry) {
        validate_resource(*scene.baked_resources);reuse(scene.baked_resources);
    }
    for(const auto& r:cache)reuse(r);
    const auto list=instances(scene);
    const auto vertices=vertex_count(scene,list),triangles_count=triangle_count(scene,list);
    const std::uint64_t cells=std::uint64_t(s.sdf_resolution)*s.sdf_resolution*s.sdf_resolution;
    if(s.environment_diffuse==EnvironmentDiffuse::prt&&!result->prt&&vertices*std::uint64_t(s.bake_samples)>max_prt_rays)
        throw std::length_error("Scene PRT exceeds 8M visibility-ray budget");
    if(vertices*sizeof(Sh9)+(s.sdf_shadows?cells*sizeof(float):0)>max_bytes)
        throw std::length_error("Scene bake exceeds 64MiB byte budget");
    result->environment_sh9=project_environment(scene);
    const bool need_prt=s.environment_diffuse==EnvironmentDiffuse::prt&&!result->prt;
    const bool need_sdf=s.sdf_shadows&&!result->sdf&&triangles_count>0;
    try { if(need_prt||need_sdf) {
        const auto triangles=flatten_scene(scene);
        if(need_prt) {
            const Bvh bvh(triangles);result->prt=bake_prt(scene,list,bvh,s.bake_samples,cancel);
            if(cancelled(cancel))return {};
        }
        if(need_sdf) {
            result->sdf=bake_sdf(triangles,s.sdf_resolution,cancel);
            if(cancelled(cancel))return {};
        }
    } }catch(const BakeCancelled&) { return {}; }
    if(cancelled(cancel))return {};
    validate_resource(*result);
    std::uint64_t bytes=resource_bytes(*result);
    for(const auto& r:cache)bytes+=resource_bytes(*r);
    while(!cache.empty()&&(cache.size()>=4||bytes>max_cache_bytes)) {
        bytes-=resource_bytes(*cache.front());cache.erase(cache.begin());
    }
    cache.push_back(result);return result;
}
glm::vec3 evaluate_environment_sh(const EnvironmentSh9& sh,glm::vec3 normal) {
    // 辐照度卷积包含 pi，最终材质漫反射只再乘 albedo/pi 一次。
    return glm::max(evaluate_sh9_irradiance(sh,normal),glm::vec3(0));
}
glm::vec3 evaluate_prt_irradiance(const EnvironmentSh9& sh,const Sh9& transfer) {
    glm::vec3 result(0);for(int k=0;k<9;++k)result+=sh[k]*transfer[k];return glm::max(result,glm::vec3(0));
}
Scene apply_scene_resources(const Scene& scene,std::shared_ptr<const SceneBakeResources> resources,const Settings& s) {
    if(!resources)throw std::invalid_argument("Missing scene resource snapshot");
    validate_resource(*resources);
    if(resources->geometry_hash!=geometry_fingerprint(scene))throw std::invalid_argument("Stale scene geometry bake");
    // 独立 API 也支持天空更新；只更新 SH 小壳，不重新计算静态 transfer。
    const auto env=environment_hash(scene);
    if(resources->environment_hash!=env) {
        auto relit=std::make_shared<SceneBakeResources>(*resources);relit->environment_hash=env;
        relit->environment_sh9=project_environment(scene);resources=relit;
    }
    Scene out=scene;out.baked_resources=resources;
    if(s.environment_diffuse!=EnvironmentDiffuse::prt)return out;
    if(!resources->prt||resources->bake_samples!=s.bake_samples)throw std::invalid_argument("Missing/mismatched PRT transfer");
    const auto list=instances(scene);
    if(resources->prt->instances.size()!=list.size())throw std::invalid_argument("PRT instance layout mismatch");
    std::vector<bool> used(scene.meshes.size(),false);
    for(std::size_t i=0;i<list.size();++i) {
        const auto& instance=list[i];const auto& transfer=resources->prt->instances[i];
        const auto source=std::size_t(instance.mesh);
        if(transfer.node!=instance.node||transfer.transfer.size()!=scene.meshes[source].vertices.size())
            throw std::invalid_argument("PRT vertex/instance layout mismatch");
        Mesh mesh=scene.meshes[source];
        for(std::size_t j=0;j<mesh.vertices.size();++j)
            mesh.vertices[j].baked_irradiance=evaluate_prt_irradiance(resources->environment_sh9,transfer.transfer[j]);
        if(s.auto_lod&&!mesh.lods.empty()) {
            // 持久化 LOD 只有派生顶点，没有原顶点对应表；最近原三角面重心映射照度。
            // 此步骤仅重映射已有 transfer，不依赖相机、不重新发射可见性射线。
            Scene local;local.meshes.push_back(mesh);local.meshes[0].lods.clear();
            const auto triangles=flatten_scene(local);const DistanceBvh nearest(triangles,nullptr);DistanceBudget budget;
            std::uint64_t lod_vertices=0;
            for(auto& lod:mesh.lods) {
                lod_vertices+=lod.vertices.size();
                if(lod_vertices>max_vertices)throw std::length_error("PRT LOD remap vertex budget exceeded");
                for(auto& v:lod.vertices) {
                    if(!finite(v.position))throw std::invalid_argument("Invalid PRT LOD vertex");
                    const auto point=nearest.nearest(glm::dvec3(v.position),budget);v.baked_irradiance=glm::vec3(0);
                    if(point.triangle>=0)for(int k=0;k<3;++k)
                        v.baked_irradiance+=float(point.barycentric[k])*triangles[std::size_t(point.triangle)].vertices[k].baked_irradiance;
                }
            }
        }
        // 同一 mesh 的不同世界实例必须各有一份 transfer，节点层级/材料/UV 不变。
        int target=instance.mesh;
        if(used[source]) { target=int(out.meshes.size());out.meshes.push_back(std::move(mesh)); }
        else { out.meshes[source]=std::move(mesh);used[source]=true; }
        if(instance.node>=0)out.nodes[std::size_t(instance.node)].mesh=target;
    }
    return out;
}
float sample_scene_sdf(const SceneSdfGrid& g,glm::vec3 p) {
    if(!finite(p)||g.resolution<4||g.resolution>64||!finite(g.min)||!finite(g.max)||
       glm::any(glm::lessThanEqual(g.max,g.min))||
       g.values.size()!=std::uint64_t(g.resolution)*g.resolution*g.resolution)
        throw std::invalid_argument("Invalid scene SDF query");
    const auto clamped=glm::clamp(p,g.min,g.max);
    if(clamped!=p)return glm::length(p-clamped); // 网格外到包围盒的保守下界。
    const auto step=(g.max-g.min)/float(g.resolution-1);
    const auto q=glm::clamp((p-g.min)/step,glm::vec3(0),glm::vec3(float(g.resolution-1)));
    const auto cell=glm::min(glm::ivec3(glm::floor(q)),glm::ivec3(g.resolution-2));
    const auto f=q-glm::vec3(cell);double value=0,error=0;
    for(int z=0;z<2;++z)for(int y=0;y<2;++y)for(int x=0;x<2;++x) {
        const double weight=double(x?f.x:1-f.x)*(y?f.y:1-f.y)*(z?f.z:1-f.z);
        const glm::ivec3 corner=cell+glm::ivec3(x,y,z);
        const float distance=g.values[(std::size_t(corner.z)*g.resolution+corner.y)*g.resolution+corner.x];
        if(!std::isfinite(distance)||distance<0)throw std::invalid_argument("Invalid scene SDF query value");
        value+=weight*distance;
        // 距离场是 1-Lipschitz；扣除各格点覆盖距离的加权和后绝不会越过真实表面。
        error+=weight*glm::length(glm::dvec3(g.min)+glm::dvec3(step)*glm::dvec3(corner)-glm::dvec3(p));
    }
    return float(std::max(0.,value-error));
}
float scene_sdf_visibility(const SceneSdfGrid& g,glm::vec3 position,glm::vec3 normal,
                           glm::vec3 direction,float max_distance,float softness,float bias) {
    if(!finite(position)||!finite(normal)||!finite(direction)||!std::isfinite(softness)||softness<=0||
       !std::isfinite(bias)||bias<0||!std::isfinite(max_distance)||max_distance<=0||glm::dot(direction,direction)==0)
        throw std::invalid_argument("Invalid scene SDF shadow ray");
    if(g.resolution<4||g.resolution>64||!finite(g.min)||!finite(g.max)||glm::any(glm::lessThanEqual(g.max,g.min)))
        throw std::invalid_argument("Invalid scene SDF shadow grid");
    direction=safe_normalize(direction);normal=safe_normalize(normal);
    const float diagonal=glm::length((g.max-g.min)/float(g.resolution-1));
    // 低分辨率保守插值扩大表面，起点移出一个格点覆盖半径；因此近接触阴影有格距级偏差。
    const float offset=std::max(bias,diagonal*1.05f),epsilon=std::max(1e-5f,diagonal*1e-3f);
    const Ray ray{position+normal*offset,direction,epsilon,std::max(epsilon,max_distance-offset)};
    Aabb bounds;bounds.min=g.min;bounds.max=g.max;double enter=0,exit=0;
    if(!bounds.intersect(ray,enter,exit))return 1;
    float t=std::max(epsilon,float(enter)),visibility=1;
    for(int i=0;i<512&&t<=float(exit);++i) {
        const float d=sample_scene_sdf(g,ray.origin+direction*t);
        if(d<=epsilon)return 0;
        visibility=std::min(visibility,softness*d/std::max(t,offset));
        t+=std::max(epsilon,d*.9f);
    }
    // 未走出包围盒时保守判遮挡，不能把步数耗尽伪装为完全可见。
    return t<=float(exit)?0.f:std::clamp(visibility,0.f,1.f);
}
std::string serialize_scene_resources(const SceneBakeResources& r) {
    validate_resource(r);Writer payload;
    payload.u64(r.geometry_hash);payload.u64(r.environment_hash);payload.u32(std::uint32_t(r.bake_samples));
    for(auto c:r.environment_sh9)payload.vector(c);
    payload.u32(r.prt?1u:0u);
    if(r.prt) {
        payload.u32(std::uint32_t(r.prt->instances.size()));
        for(const auto& i:r.prt->instances) {
            payload.u32(std::uint32_t(i.node+1));payload.u32(std::uint32_t(i.mesh));payload.u32(std::uint32_t(i.transfer.size()));
            for(const auto& transfer:i.transfer)for(float x:transfer)payload.scalar(x);
        }
    }
    payload.u32(r.sdf?1u:0u);
    if(r.sdf) {
        payload.vector(r.sdf->min);payload.vector(r.sdf->max);payload.u32(std::uint32_t(r.sdf->resolution));
        payload.u32(1);payload.u32(std::uint32_t(r.sdf->values.size()));for(float x:r.sdf->values)payload.scalar(x);
    }
    Writer out;out.data="EFBK";out.u32(1);out.u64(payload.data.size());out.u64(bytes_hash(payload.data));out.data+=payload.data;return out.data;
}
std::shared_ptr<const SceneBakeResources> deserialize_scene_resources(const std::string& bytes) {
    if(bytes.size()<24||bytes.size()>max_bytes+24||bytes.substr(0,4)!="EFBK")
        throw std::invalid_argument("Invalid scene resource envelope");
    Reader header{std::string_view(bytes).substr(4,20)};
    if(header.u32()!=1)throw std::invalid_argument("Unsupported scene resource version");
    const auto length=header.u64(),checksum=header.u64();const std::string_view body=std::string_view(bytes).substr(24);
    if(length!=body.size()||bytes_hash(body)!=checksum)throw std::invalid_argument("Scene resource length/checksum mismatch");
    Reader in{body};auto r=std::make_shared<SceneBakeResources>();
    r->geometry_hash=in.u64();r->environment_hash=in.u64();const auto samples=in.u32();
    if(samples<16||samples>4096)throw std::invalid_argument("Invalid serialized sample count");
    r->bake_samples=int(samples);
    for(auto& c:r->environment_sh9)c=in.vector();
    const auto has_prt=in.u32();if(has_prt>1)throw std::invalid_argument("Invalid PRT presence flag");
    if(has_prt) {
        auto prt=std::make_shared<ScenePrtResources>();const auto count=in.u32();
        if(count>max_instances||count>(body.size()-in.offset)/12)throw std::invalid_argument("Invalid serialized PRT instances");
        prt->instances.reserve(count);std::uint64_t vertices=0;
        for(std::uint32_t i=0;i<count;++i) {
            ScenePrtInstance instance;const auto node=in.u32(),mesh=in.u32(),size=in.u32();vertices+=size;
            if(node>max_instances||mesh>=max_instances||vertices>max_vertices||size>(body.size()-in.offset)/sizeof(Sh9))
                throw std::invalid_argument("Invalid serialized PRT layout");
            instance.node=int(node)-1;instance.mesh=int(mesh);instance.transfer.resize(size);
            for(auto& transfer:instance.transfer)for(float& x:transfer)x=in.scalar();
            prt->instances.push_back(std::move(instance));
        }
        r->prt=prt;
    }
    const auto has_sdf=in.u32();if(has_sdf>1)throw std::invalid_argument("Invalid SDF presence flag");
    if(has_sdf) {
        auto sdf=std::make_shared<SceneSdfGrid>();sdf->min=in.vector();sdf->max=in.vector();
        const auto resolution=in.u32(),unsigned_distance=in.u32(),count=in.u32();
        if(resolution<4||resolution>64||unsigned_distance!=1||count!=std::uint64_t(resolution)*resolution*resolution||
           count>(body.size()-in.offset)/4)throw std::invalid_argument("Invalid serialized SDF layout");
        sdf->resolution=int(resolution);sdf->values.resize(count);for(float& x:sdf->values)x=in.scalar();r->sdf=sdf;
    }
    if(in.offset!=body.size())throw std::invalid_argument("Trailing scene resource data");
    validate_resource(*r);return r;
}
} // namespace emberframe::lab
