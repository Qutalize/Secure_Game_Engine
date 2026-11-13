// Build-only FBX importer. No FBX code or original source files enter the player.
#include "ufbx.h"
#include <array>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>
#include <windows.h>
using Scene=std::unique_ptr<ufbx_scene,decltype(&ufbx_free_scene)>;
void Require(bool ok,const char* message){if(!ok)throw std::runtime_error(message);}
std::string Utf8(const wchar_t* text){int n=WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,text,-1,nullptr,0,nullptr,nullptr);Require(n>0,"Invalid UTF-16 argument");std::string out(n,'\0');WideCharToMultiByte(CP_UTF8,0,text,-1,out.data(),n,nullptr,nullptr);out.pop_back();return out;}
void String(std::ostream& out,const std::string& s){out<<'"';for(unsigned char c:s){if(c=='"'||c=='\\')out<<'\\'<<c;else if(c<32)out<<"\\u00"<<std::hex<<std::setw(2)<<std::setfill('0')<<int(c)<<std::dec;else out<<c;}out<<'"';}
std::string Name(ufbx_string name){return {name.data,name.length};}
using Matrix=std::array<double,16>;
Matrix Convert(const ufbx_matrix& m){return {m.m00,m.m10,m.m20,0,m.m01,m.m11,m.m21,0,m.m02,m.m12,m.m22,0,m.m03,m.m13,m.m23,1};}
struct Vertex{ufbx_vec3 position;std::array<unsigned,4> joints{};std::array<double,4> weights{};};
struct Binding{bool cluster;uint32_t id;};
std::vector<Matrix> Palette(ufbx_scene* scene,const std::vector<Binding>& bindings){std::vector<Matrix> result;for(auto b:bindings)result.push_back(Convert(b.cluster?scene->skin_clusters[b.id]->geometry_to_world:scene->nodes[b.id]->geometry_to_world));return result;}
std::array<double,3> Transform(const Vertex& v,const std::vector<Matrix>& matrices){std::array<double,3> out{};for(int k=0;k<4;k++){auto m=matrices[v.joints[k]];for(int a=0;a<3;a++)out[a]+=(m[a]*v.position.x+m[4+a]*v.position.y+m[8+a]*v.position.z+m[12+a])*v.weights[k];}return out;}
template<class T> void Array(std::ostream& out,const T& values){out<<'[';bool first=true;for(auto v:values){if(!first)out<<',';first=false;Require(std::isfinite(double(v)),"Non-finite FBX data");out<<v;}out<<']';}
int wmain(int argc,wchar_t** argv){try{
    Require(argc==5,"Usage: fbx_cooker source.fbx output.json static|skeletal clip-name");bool animate=std::wstring(argv[3])==L"skeletal";auto clip=Utf8(argv[4]);
    auto source=std::filesystem::path(argv[1]);auto size=std::filesystem::file_size(source);Require(size>0&&size<=8*1024*1024,"FBX source exceeds 8 MiB");
    std::ifstream input(source,std::ios::binary);std::vector<char> bytes(size);input.read(bytes.data(),bytes.size());Require(bool(input),"Cannot read FBX");
    ufbx_load_opts opts{};opts.file_format=UFBX_FILE_FORMAT_FBX;opts.ignore_embedded=true;opts.ignore_animation=!animate;opts.strict=true;
    opts.target_axes=ufbx_axes_right_handed_y_up;opts.target_unit_meters=1;
    opts.temp_allocator.memory_limit=128*1024*1024;opts.result_allocator.memory_limit=128*1024*1024;
    ufbx_error error{};Scene scene(ufbx_load_memory(bytes.data(),bytes.size(),&opts,&error),ufbx_free_scene);Require(bool(scene),"Invalid/unsupported FBX (ufbx load failed)");
    Require(scene->nodes.count<=512,"FBX exceeds 512 nodes");
    std::vector<Vertex> vertices;std::vector<uint32_t> indices;std::vector<Binding> bindings;bool hasSkin=false;
    for(auto node:scene->nodes){auto mesh=node->mesh;if(!mesh)continue;
        Require(mesh->blend_deformers.count==0,"FBX morph deformers are unsupported");Require(mesh->skin_deformers.count<=1,"Multiple FBX skin deformers are unsupported");
        auto offset=unsigned(bindings.size());auto skin=mesh->skin_deformers.count?mesh->skin_deformers[0]:nullptr;
        if(skin){hasSkin=true;Require(skin->skinning_method==UFBX_SKINNING_METHOD_LINEAR||skin->skinning_method==UFBX_SKINNING_METHOD_RIGID,"FBX dual-quaternion skinning is unsupported; export linear weights");
            for(auto cluster:skin->clusters)bindings.push_back({true,cluster->typed_id});
        }else bindings.push_back({false,node->typed_id});
        Require(bindings.size()<=256,"FBX exceeds 256 palette entries");
        auto begin=uint32_t(vertices.size());Require(vertices.size()+mesh->num_vertices<=65536,"FBX exceeds 65536 vertices");
        for(size_t i=0;i<mesh->num_vertices;i++){Vertex v{};v.position=mesh->vertices[i];
            if(skin){auto sv=skin->vertices[i];Require(sv.num_weights>0&&sv.num_weights<=4,"FBX needs 1..4 weights per vertex; no implicit weight truncation");double sum=0;
                for(unsigned j=0;j<sv.num_weights;j++){auto w=skin->weights[sv.weight_begin+j];Require(w.cluster_index<skin->clusters.count&&std::isfinite(w.weight)&&w.weight>=0,"Invalid FBX skin weight");v.joints[j]=offset+w.cluster_index;v.weights[j]=w.weight;sum+=w.weight;}
                Require(sum>1e-8,"Zero FBX skin weights");for(auto& w:v.weights)w/=sum;
            }else{v.joints.fill(offset);v.weights[0]=1;}vertices.push_back(v);
        }
        for(auto face:mesh->faces){Require(face.num_indices>=3&&face.num_indices<=65536,"Invalid FBX polygon size");std::vector<uint32_t> corners((face.num_indices-2)*3);
            auto count=ufbx_triangulate_face(corners.data(),corners.size(),mesh,face);Require(count>0,"Cannot triangulate FBX face");
            Require(indices.size()+count*3<=196608,"FBX exceeds 65536 triangles");
            for(uint32_t i=0;i<count*3;i++)indices.push_back(begin+mesh->vertex_indices[corners[i]]);
        }
    }
    Require(!indices.empty(),"FBX has no triangles");
    std::vector<std::string> names;for(auto stack:scene->anim_stacks)names.push_back(Name(stack->name));
    const ufbx_anim_stack* selected=nullptr;double duration=0;
    if(animate){Require(hasSkin&&!names.empty(),"Skeletal FBX requires a skin and animation stack");
        if(clip.empty())selected=scene->anim_stacks[0];else for(auto stack:scene->anim_stacks)if(Name(stack->name)==clip){Require(!selected,"Ambiguous FBX clip name");selected=stack;}
        Require(selected!=nullptr,"FBX animation clip not found");duration=selected->time_end-selected->time_begin;
        Require(std::isfinite(duration)&&duration>0&&duration<=60,"FBX clip duration must be 0..60 seconds");
    }
    unsigned count=animate?unsigned(std::ceil(duration*60))+1:1;Require(count<=3601,"FBX sample limit");
    std::vector<std::vector<Matrix>> frames;frames.reserve(count);
    for(unsigned f=0;f<count;f++){
        if(animate){ufbx_evaluate_opts eval{};eval.temp_allocator.memory_limit=128*1024*1024;eval.result_allocator.memory_limit=128*1024*1024;
            Scene evaluated(ufbx_evaluate_scene(scene.get(),selected->anim,selected->time_begin+duration*f/(count-1),&eval,&error),ufbx_free_scene);Require(bool(evaluated),"FBX evaluation failed");frames.push_back(Palette(evaluated.get(),bindings));
        }else frames.push_back(Palette(scene.get(),bindings));
    }
    Scene rigScene(nullptr,ufbx_free_scene);
    if(animate){ufbx_evaluate_opts eval{};eval.temp_allocator.memory_limit=128*1024*1024;eval.result_allocator.memory_limit=128*1024*1024;
        rigScene.reset(ufbx_evaluate_scene(scene.get(),selected->anim,selected->time_begin,&eval,&error));Require(bool(rigScene),"FBX initial rig evaluation failed");}
    auto initial=rigScene?rigScene.get():scene.get();std::vector<ufbx_node*> bones;
    for(auto b:bindings)bones.push_back(b.cluster?initial->skin_clusters[b.id]->bone_node:initial->nodes[b.id]);
    std::ofstream out(std::filesystem::path(argv[2]),std::ios::binary);out<<std::setprecision(9);Require(bool(out),"Cannot write cooked FBX");
    out<<"{\"positions\":[";for(size_t i=0;i<vertices.size();i++){if(i)out<<',';Array(out,Transform(vertices[i],frames[0]));}
    out<<"],\"indices\":";Array(out,indices);out<<",\"base\":[";
    for(size_t i=0;i<vertices.size();i++){if(i)out<<',';auto p=vertices[i].position;Array(out,std::array<double,3>{p.x,p.y,p.z});}
    out<<"],\"joints\":[";for(size_t i=0;i<vertices.size();i++){if(i)out<<',';Array(out,vertices[i].joints);}
    out<<"],\"weights\":[";for(size_t i=0;i<vertices.size();i++){if(i)out<<',';Array(out,vertices[i].weights);}
    out<<"],\"frames\":[";for(size_t i=0;i<frames.size();i++){if(i)out<<',';out<<'[';for(size_t j=0;j<frames[i].size();j++){if(j)out<<',';Array(out,frames[i][j]);}out<<']';}
    out<<"],\"duration\":"<<duration<<",\"clip\":";String(out,selected?Name(selected->name):"");out<<",\"clips\":[";
    for(size_t i=0;i<names.size();i++){if(i)out<<',';String(out,names[i]);}out<<"],\"rig\":[";
    for(size_t i=0;i<bones.size();++i){if(i)out<<',';auto node=bones[i];Require(node!=nullptr,"FBX missing skin bone");int parent=-1;
        for(auto ancestor=node->parent;ancestor&&parent<0;ancestor=ancestor->parent)
            for(size_t j=0;j<bones.size();++j)if(bones[j]==ancestor){parent=int(j);break;}
        auto anchor=node->node_to_world.cols[3];out<<"{\"parent\":"<<parent<<",\"node\":"<<node->typed_id<<",\"anchor\":";
        Array(out,std::array<double,3>{anchor.x,anchor.y,anchor.z});out<<'}';}
    out<<"]}";Require(bool(out),"Writing FBX output failed");return 0;
}catch(const std::exception& e){std::cerr<<"FBX cooker: "<<e.what()<<'\n';return 2;}}
