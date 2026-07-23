#ifndef slic3r_Format_OBJ_hpp_
#define slic3r_Format_OBJ_hpp_
#include "libslic3r/Color.hpp"
#include <unordered_map>
#include <vector>
namespace Slic3r {

class TriangleMesh;
class Model;
class ModelObject;

struct TriangleColor
{
    int pid{-1};
    int indices[3]{-1, -1, -1};
};

struct VolumeColorInfo
{
    int pid{-1};
    int pindex{-1};
    std::vector<TriangleColor> triangle_colors;
};

using VolumeColorInfoMap = std::unordered_map<int, VolumeColorInfo>;

// Load an OBJ file into a provided model.
struct ObjInfo {
    std::vector<RGBA> vertex_colors;
    std::vector<RGBA> face_colors;
    bool              is_single_mtl{false};
    std::string       lost_material_name{""};
    std::vector<std::array<Vec2f,3>> uvs;
    std::string        obj_dircetory;
    std::map<std::string,bool>  pngs;
    std::unordered_map<int, std::string> uv_map_pngs;
    bool              has_uv_png{false};

};
struct ObjDialogInOut
{ // input:colors array
    enum class FormatType { Obj, Standard3mf };

    std::vector<RGBA> input_colors;
    bool              is_single_color{false};
    // colors array output:
    std::vector<unsigned char> filament_ids;
    unsigned char              first_extruder_id;
    bool                       deal_vertex_color;
    Model *                    model{nullptr};
    std::string lost_material_name{""};
    FormatType input_type{FormatType::Obj};
    std::unordered_map<int, std::vector<RGBA>> color_group_map;
    VolumeColorInfoMap volume_colors;
    std::vector<bool> filament_available_on_device;
};

typedef std::function<void(ObjDialogInOut &in_out)> ObjImportColorFn;
extern bool load_obj(const char *path, TriangleMesh *mesh, ObjInfo &vertex_colors, std::string &message);
extern bool load_obj(const char *path, Model *model, ObjInfo &vertex_colors, std::string &message, const char *object_name = nullptr);

extern bool store_obj(const char *path, TriangleMesh *mesh);
extern bool store_obj(const char *path, ModelObject *model);
extern bool store_obj(const char *path, Model *model);

}; // namespace Slic3r

#endif /* slic3r_Format_OBJ_hpp_ */
