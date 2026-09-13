#include <long_march.h>

#include <embree4/rtcore.h>
#include <rapidjson/document.h>
#include <rapidjson/error/en.h>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image.h>
#include <stb_image_write.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <glm/gtc/matrix_transform.hpp>

namespace {
using Json = rapidjson::Value;
using Vec2 = glm::vec2;
using Vec3 = glm::vec3;
using Vec4 = glm::vec4;
constexpr float kPi = 3.14159265358979323846f;
constexpr float kEpsilon = 1.0e-4f;

float MaxComponent(const Vec3 &v) { return std::max(v.x, std::max(v.y, v.z)); }
Vec3 SafeNormalize(const Vec3 &v) {
  float length2 = glm::dot(v, v);
  return length2 > 1.0e-20f ? v / std::sqrt(length2) : Vec3(0.0f, 1.0f, 0.0f);
}
Vec3 Vec3Value(const Json &value, Vec3 fallback = Vec3(0.0f)) {
  if (!value.IsArray() || value.Size() != 3) return fallback;
  return {value[0].GetFloat(), value[1].GetFloat(), value[2].GetFloat()};
}
Vec3 Vec3Member(const Json &value, const char *name, Vec3 fallback = Vec3(0.0f)) {
  return value.HasMember(name) ? Vec3Value(value[name], fallback) : fallback;
}
float FloatMember(const Json &value, const char *name, float fallback) {
  return value.HasMember(name) ? value[name].GetFloat() : fallback;
}
bool BoolMember(const Json &value, const char *name, bool fallback) {
  return value.HasMember(name) ? value[name].GetBool() : fallback;
}
const Json &Required(const Json &value, const char *name) {
  if (!value.IsObject() || !value.HasMember(name)) throw std::runtime_error(std::string("missing field: ") + name);
  return value[name];
}

struct Random {
  uint64_t state;
  explicit Random(uint64_t seed) : state(seed + 0x9e3779b97f4a7c15ULL) {}
  uint32_t NextUInt() {
    state ^= state >> 12;
    state ^= state << 25;
    state ^= state >> 27;
    return uint32_t((state * 2685821657736338717ULL) >> 32);
  }
  float Next() { return (NextUInt() + 0.5f) * (1.0f / 4294967296.0f); }
};

struct Texture {
  int width{}, height{}, channels{};
  std::vector<uint8_t> pixels;

  explicit operator bool() const { return !pixels.empty(); }
  Vec3 Sample(Vec2 uv) const {
    if (pixels.empty()) return Vec3(1.0f);
    uv -= glm::floor(uv);
    float x = uv.x * width - 0.5f;
    // Sparkium's shader flips V before sampling textures loaded from image files.
    float y = (1.0f - uv.y) * height - 0.5f;
    int x0 = static_cast<int>(std::floor(x));
    int y0 = static_cast<int>(std::floor(y));
    float tx = x - std::floor(x), ty = y - std::floor(y);
    auto texel = [&](int px, int py) {
      px = (px % width + width) % width;
      py = (py % height + height) % height;
      const uint8_t *p = pixels.data() + (py * width + px) * channels;
      return Vec3(p[0], channels > 1 ? p[1] : p[0], channels > 2 ? p[2] : p[0]) / 255.0f;
    };
    return glm::mix(glm::mix(texel(x0, y0), texel(x0 + 1, y0), tx),
                    glm::mix(texel(x0, y0 + 1), texel(x0 + 1, y0 + 1), tx), ty);
  }
};

enum class MaterialType { Lambertian, Specular, Principled, Light };
struct Material {
  MaterialType type{MaterialType::Lambertian};
  Vec3 base_color{0.8f};
  Vec3 emission{0.0f};
  float metallic{0.0f};
  float specular{0.0f};
  float roughness{0.5f};
  float anisotropic{0.0f};
  float anisotropic_rotation{0.0f};
  float transmission{0.0f};
  float transmission_roughness{0.0f};
  float ior{1.45f};
  bool two_sided{false};
  bool block_ray{false};
  Texture base_color_texture, metallic_texture, specular_texture, roughness_texture;
  Texture anisotropic_texture, anisotropic_rotation_texture, normal_texture;
};

struct Vertex { Vec3 position, normal, tangent; Vec2 uv; };
struct Triangle {
  uint32_t vertex[3]{};
  int material{};
  Vec3 geom_normal{};
  float area{};
  float local_area{};
};
struct PointLight { Vec3 position, power; float selection_power{}; };
struct AreaLight {
  std::vector<uint32_t> triangles;
  std::vector<float> area_cdf;
  float total_area{};
  float selection_power{};
  int material{};
};
struct Hit {
  float distance{};
  uint32_t triangle{};
  float u{}, v{};
  Vec3 position{}, normal{}, geom_normal{}, tangent{};
  Vec2 uv{};
  bool front_facing{};
};

struct MeshData {
  std::vector<Vertex> vertices;
  std::vector<uint32_t> indices;
};

glm::mat4 ParseTransform(const Json &value) {
  glm::mat4 result(1.0f);
  if (!value.IsObject()) return result;
  if (value.HasMember("matrix")) {
    const auto &matrix = value["matrix"];
    if (!matrix.IsArray() || matrix.Size() != 16) throw std::runtime_error("transform.matrix needs 16 values");
    for (rapidjson::SizeType i = 0; i < 16; ++i) result[i / 4][i % 4] = matrix[i].GetFloat();
    return result;
  }
  result = glm::translate(result, Vec3Member(value, "translation"));
  Vec3 rotation = glm::radians(Vec3Member(value, "rotation_degrees"));
  result = glm::rotate(result, rotation.x, Vec3(1, 0, 0));
  result = glm::rotate(result, rotation.y, Vec3(0, 1, 0));
  result = glm::rotate(result, rotation.z, Vec3(0, 0, 1));
  return glm::scale(result, Vec3Member(value, "scale", Vec3(1.0f)));
}

MeshData LoadMesh(const Json &spec, const std::filesystem::path &scene_path) {
  grassland::Mesh<float> mesh;
  std::string type = Required(spec, "type").GetString();
  if (type == "sphere") {
    int longitude = spec.HasMember("longitude_segments") ? spec["longitude_segments"].GetInt() : 30;
    int latitude = spec.HasMember("latitude_segments") ? spec["latitude_segments"].GetInt() : -1;
    mesh = grassland::Mesh<float>::Sphere(longitude, latitude);
  } else if (type == "mesh") {
    auto path = std::filesystem::path(Required(spec, "path").GetString());
    if (path.is_relative()) path = scene_path.parent_path() / path;
    if (mesh.LoadObjFile(path.lexically_normal().string()) != 0) throw std::runtime_error("cannot load mesh: " + path.string());
  } else if (type == "inline_mesh") {
    std::vector<grassland::Vector3<float>> positions;
    std::vector<grassland::Vector2<float>> uv;
    std::vector<uint32_t> indices;
    for (const auto &p : Required(spec, "positions").GetArray()) {
      Vec3 v = Vec3Value(p); positions.emplace_back(v.x, v.y, v.z);
    }
    for (const auto &i : Required(spec, "indices").GetArray()) indices.push_back(i.GetUint());
    if (spec.HasMember("tex_coords"))
      for (const auto &p : spec["tex_coords"].GetArray()) uv.emplace_back(p[0].GetFloat(), p[1].GetFloat());
    mesh = grassland::Mesh<float>(positions.size(), indices.size(), indices.data(), positions.data(), nullptr,
                                  uv.empty() ? nullptr : uv.data());
  } else {
    throw std::runtime_error("unknown geometry type: " + type);
  }
  if (!mesh.Normals()) mesh.GenerateNormals(-1.0f);
  if (mesh.TexCoords() && !mesh.Tangents()) mesh.GenerateTangents();
  MeshData result;
  result.vertices.resize(mesh.NumVertices());
  for (size_t i = 0; i < mesh.NumVertices(); ++i) {
    auto p = mesh.Positions()[i];
    auto n = mesh.Normals()[i];
    result.vertices[i].position = {p.x(), p.y(), p.z()};
    result.vertices[i].normal = {n.x(), n.y(), n.z()};
    if (mesh.TexCoords()) {
      auto t = mesh.TexCoords()[i]; result.vertices[i].uv = {t.x(), t.y()};
    }
    if (mesh.Tangents()) {
      auto t = mesh.Tangents()[i]; result.vertices[i].tangent = {t.x(), t.y(), t.z()};
    }
  }
  result.indices.assign(mesh.Indices(), mesh.Indices() + mesh.NumIndices());
  return result;
}

Texture LoadTexture(const Json &textures, const char *slot, const std::filesystem::path &scene_path) {
  Texture result;
  if (!textures.IsObject() || !textures.HasMember(slot)) return result;
  auto path = std::filesystem::path(textures[slot].GetString());
  if (path.is_relative()) path = scene_path.parent_path() / path;
  int width, height, channels;
  unsigned char *data = stbi_load(path.string().c_str(), &width, &height, &channels, 4);
  if (!data) throw std::runtime_error("cannot load texture: " + path.string());
  result.width = width; result.height = height; result.channels = 4;
  result.pixels.assign(data, data + static_cast<size_t>(width) * height * 4);
  stbi_image_free(data);
  return result;
}

struct CpuScene {
  std::string name;
  int width{}, height{}, max_bounces{32};
  float persistence{1.0f}, clamping{100.0f}, max_exposure{1.0f};
  Vec3 eye{}, target{}, up{0, 1, 0};
  float fov{60.0f};
  std::vector<Material> materials;
  std::vector<Vertex> vertices;
  std::vector<Triangle> triangles;
  std::vector<PointLight> point_lights;
  std::vector<AreaLight> area_lights;
  std::vector<int> triangle_light;
  float total_light_power{};
  RTCDevice device{};
  RTCScene scene{};

  ~CpuScene() { if (scene) rtcReleaseScene(scene); if (device) rtcReleaseDevice(device); }
  CpuScene() = default;
  CpuScene(const CpuScene &) = delete;
  CpuScene &operator=(const CpuScene &) = delete;
  CpuScene(CpuScene &&other) noexcept
      : name(std::move(other.name)),
        width(other.width), height(other.height), max_bounces(other.max_bounces),
        persistence(other.persistence), clamping(other.clamping), max_exposure(other.max_exposure),
        eye(other.eye), target(other.target), up(other.up), fov(other.fov),
        materials(std::move(other.materials)), vertices(std::move(other.vertices)),
        triangles(std::move(other.triangles)), point_lights(std::move(other.point_lights)),
        area_lights(std::move(other.area_lights)), triangle_light(std::move(other.triangle_light)),
        total_light_power(other.total_light_power),
        device(std::exchange(other.device, nullptr)), scene(std::exchange(other.scene, nullptr)) {}

  void BuildAcceleration() {
    device = rtcNewDevice(nullptr);
    if (!device) throw std::runtime_error("failed to create Embree device");
    scene = rtcNewScene(device);
    RTCGeometry geometry = rtcNewGeometry(device, RTC_GEOMETRY_TYPE_TRIANGLE);
    struct Position { float x, y, z; };
    struct Index { uint32_t x, y, z; };
    auto *positions = static_cast<Position *>(rtcSetNewGeometryBuffer(
        geometry, RTC_BUFFER_TYPE_VERTEX, 0, RTC_FORMAT_FLOAT3, sizeof(Position), vertices.size()));
    auto *indices = static_cast<Index *>(rtcSetNewGeometryBuffer(
        geometry, RTC_BUFFER_TYPE_INDEX, 0, RTC_FORMAT_UINT3, sizeof(Index), triangles.size()));
    for (size_t i = 0; i < vertices.size(); ++i)
      positions[i] = {vertices[i].position.x, vertices[i].position.y, vertices[i].position.z};
    for (size_t i = 0; i < triangles.size(); ++i)
      indices[i] = {triangles[i].vertex[0], triangles[i].vertex[1], triangles[i].vertex[2]};
    rtcCommitGeometry(geometry);
    rtcAttachGeometry(scene, geometry);
    rtcReleaseGeometry(geometry);
    rtcCommitScene(scene);
  }

  bool Intersect(const Vec3 &origin, const Vec3 &direction, float maximum, Hit &hit) const {
    RTCRayHit query{};
    query.ray.org_x = origin.x; query.ray.org_y = origin.y; query.ray.org_z = origin.z;
    query.ray.dir_x = direction.x; query.ray.dir_y = direction.y; query.ray.dir_z = direction.z;
    query.ray.tnear = kEpsilon * std::max(glm::length(origin), 1.0f);
    query.ray.tfar = maximum; query.ray.mask = 0xffffffff; query.ray.flags = 0;
    query.hit.geomID = RTC_INVALID_GEOMETRY_ID;
    RTCIntersectArguments args; rtcInitIntersectArguments(&args);
    rtcIntersect1(scene, &query, &args);
    if (query.hit.geomID == RTC_INVALID_GEOMETRY_ID) return false;
    hit.distance = query.ray.tfar; hit.triangle = query.hit.primID; hit.u = query.hit.u; hit.v = query.hit.v;
    const Triangle &triangle = triangles[hit.triangle];
    float w = 1.0f - hit.u - hit.v;
    const Vertex &a = vertices[triangle.vertex[0]], &b = vertices[triangle.vertex[1]], &c = vertices[triangle.vertex[2]];
    hit.position = origin + direction * hit.distance;
    hit.geom_normal = triangle.geom_normal;
    hit.normal = SafeNormalize(a.normal * w + b.normal * hit.u + c.normal * hit.v);
    hit.tangent = SafeNormalize(a.tangent * w + b.tangent * hit.u + c.tangent * hit.v);
    hit.uv = a.uv * w + b.uv * hit.u + c.uv * hit.v;
    hit.front_facing = glm::dot(direction, hit.normal) <= 0.0f;
    if (!hit.front_facing) { hit.geom_normal = -hit.geom_normal; hit.normal = -hit.normal; hit.tangent = -hit.tangent; }
    return true;
  }

  bool Occluded(const Vec3 &origin, const Vec3 &direction, float maximum) const {
    RTCRay ray{};
    ray.org_x = origin.x; ray.org_y = origin.y; ray.org_z = origin.z;
    ray.dir_x = direction.x; ray.dir_y = direction.y; ray.dir_z = direction.z;
    ray.tnear = kEpsilon * std::max(glm::length(origin), 1.0f); ray.tfar = maximum;
    ray.mask = 0xffffffff; ray.flags = 0;
    RTCOccludedArguments args; rtcInitOccludedArguments(&args);
    rtcOccluded1(scene, &ray, &args);
    return ray.tfar < 0.0f;
  }

  bool Blocked(Vec3 origin, const Vec3 &direction, float maximum) const {
    float remaining = maximum;
    for (int step = 0; step < 16 && remaining > 0.0f; ++step) {
      Hit hit;
      if (!Intersect(origin, direction, remaining, hit)) return false;
      const Material &material = materials[triangles[hit.triangle].material];
      if (material.type != MaterialType::Light || material.block_ray) return true;
      float advance = hit.distance + kEpsilon * std::max(glm::length(hit.position), 1.0f);
      origin += direction * advance;
      remaining -= advance;
    }
    return false;
  }
};

CpuScene LoadScene(const std::filesystem::path &input_path) {
  std::ifstream stream(input_path);
  if (!stream) throw std::runtime_error("cannot open scene: " + input_path.string());
  std::stringstream buffer; buffer << stream.rdbuf();
  rapidjson::Document document;
  document.Parse<rapidjson::kParseCommentsFlag | rapidjson::kParseTrailingCommasFlag>(buffer.str().c_str());
  if (document.HasParseError()) throw std::runtime_error(rapidjson::GetParseError_En(document.GetParseError()));
  if (std::string(Required(document, "format").GetString()) != "sparkium-scene")
    throw std::runtime_error("unsupported scene format");
  CpuScene result;
  result.name = document.HasMember("name") ? document["name"].GetString() : input_path.stem().string();
  const Json &film = Required(document, "film");
  result.width = Required(film, "width").GetInt(); result.height = Required(film, "height").GetInt();
  result.persistence = FloatMember(film, "persistence", 1.0f);
  result.clamping = FloatMember(film, "clamping", 100.0f);
  result.max_exposure = FloatMember(film, "max_exposure", 1.0f);
  const Json &renderer = Required(document, "renderer");
  result.max_bounces = renderer.HasMember("max_bounces") ? renderer["max_bounces"].GetInt() : 32;
  const Json &camera = Required(document, "camera");
  result.eye = Vec3Member(camera, "eye"); result.target = Vec3Member(camera, "target");
  result.up = Vec3Member(camera, "up", Vec3(0, 1, 0)); result.fov = FloatMember(camera, "fov_degrees", 60.0f);

  std::map<std::string, int> material_ids;
  const Json &materials = Required(document, "materials");
  for (auto it = materials.MemberBegin(); it != materials.MemberEnd(); ++it) {
    Material material;
    const Json &spec = it->value;
    std::string type = Required(spec, "type").GetString();
    if (type == "lambertian") material.type = MaterialType::Lambertian;
    else if (type == "specular") material.type = MaterialType::Specular;
    else if (type == "principled") material.type = MaterialType::Principled;
    else if (type == "light") material.type = MaterialType::Light;
    else throw std::runtime_error("unknown material: " + type);
    material.base_color = Vec3Member(spec, "base_color", Vec3(0.8f));
    if (material.type == MaterialType::Principled) {
      material.emission = Vec3Member(spec, "emission_color", Vec3(1.0f)) *
                          FloatMember(spec, "emission_strength", 0.0f);
    } else {
      material.emission = Vec3Member(spec, "emission") * FloatMember(spec, "emission_strength", 1.0f);
    }
    material.metallic = FloatMember(spec, "metallic", 0.0f);
    material.specular = FloatMember(spec, "specular", 0.0f);
    material.roughness = FloatMember(spec, "roughness", 0.5f);
    material.anisotropic = FloatMember(spec, "anisotropic", 0.0f);
    material.anisotropic_rotation = FloatMember(spec, "anisotropic_rotation", 0.0f);
    material.transmission = FloatMember(spec, "transmission", 0.0f);
    material.transmission_roughness = FloatMember(spec, "transmission_roughness", 0.0f);
    material.ior = FloatMember(spec, "ior", 1.45f);
    material.two_sided = BoolMember(spec, "two_sided", false); material.block_ray = BoolMember(spec, "block_ray", false);
    if (spec.HasMember("textures")) {
      const Json &textures = spec["textures"];
      material.base_color_texture = LoadTexture(textures, "base_color", input_path);
      material.metallic_texture = LoadTexture(textures, "metallic", input_path);
      material.specular_texture = LoadTexture(textures, "specular", input_path);
      material.roughness_texture = LoadTexture(textures, "roughness", input_path);
      material.anisotropic_texture = LoadTexture(textures, "anisotropic", input_path);
      material.anisotropic_rotation_texture = LoadTexture(textures, "anisotropic_rotation", input_path);
      material.normal_texture = LoadTexture(textures, "normal", input_path);
    }
    material_ids[it->name.GetString()] = static_cast<int>(result.materials.size());
    result.materials.push_back(std::move(material));
  }

  std::map<std::string, MeshData> meshes;
  const Json &geometries = Required(document, "geometries");
  for (auto it = geometries.MemberBegin(); it != geometries.MemberEnd(); ++it)
    meshes.emplace(it->name.GetString(), LoadMesh(it->value, input_path));

  for (const Json &entity : Required(document, "entities").GetArray()) {
    std::string type = Required(entity, "type").GetString();
    if (type == "point_light") {
      Vec3 power = Vec3Member(entity, "color", Vec3(1.0f)) * FloatMember(entity, "strength", 0.0f);
      result.point_lights.push_back({Vec3Member(entity, "position"), power, MaxComponent(power)});
      continue;
    }
    if (type != "mesh") throw std::runtime_error("unknown entity type: " + type);
    const MeshData &mesh = meshes.at(Required(entity, "geometry").GetString());
    int material = material_ids.at(Required(entity, "material").GetString());
    glm::mat4 transform(1.0f);
    if (entity.HasMember("transform")) transform = ParseTransform(entity["transform"]);
    if (entity.HasMember("look_at")) {
      const Json &look = entity["look_at"];
      transform = glm::inverse(glm::lookAt(Vec3Member(look, "position"), Vec3Member(look, "target"),
                                           Vec3Member(look, "up", Vec3(0, 1, 0)))) *
                  glm::scale(glm::mat4(1.0f), Vec3Member(look, "scale", Vec3(1.0f)));
    }
    glm::mat3 normal_transform = glm::transpose(glm::inverse(glm::mat3(transform)));
    uint32_t base = static_cast<uint32_t>(result.vertices.size());
    for (const Vertex &source : mesh.vertices) {
      Vertex vertex = source;
      vertex.position = Vec3(transform * Vec4(source.position, 1.0f));
      vertex.normal = SafeNormalize(normal_transform * source.normal);
      vertex.tangent = SafeNormalize(glm::mat3(transform) * source.tangent);
      result.vertices.push_back(vertex);
    }
    AreaLight area_light;
    area_light.material = material;
    for (size_t i = 0; i < mesh.indices.size(); i += 3) {
      Triangle triangle{{base + mesh.indices[i], base + mesh.indices[i + 1], base + mesh.indices[i + 2]}, material};
      const Vec3 &a = result.vertices[triangle.vertex[0]].position;
      const Vec3 &b = result.vertices[triangle.vertex[1]].position;
      const Vec3 &c = result.vertices[triangle.vertex[2]].position;
      Vec3 cross = glm::cross(b - a, c - a);
      triangle.area = 0.5f * glm::length(cross); triangle.geom_normal = SafeNormalize(cross);
      const Vec3 &local_a = mesh.vertices[mesh.indices[i]].position;
      const Vec3 &local_b = mesh.vertices[mesh.indices[i + 1]].position;
      const Vec3 &local_c = mesh.vertices[mesh.indices[i + 2]].position;
      triangle.local_area = 0.5f * glm::length(glm::cross(local_b - local_a, local_c - local_a));
      uint32_t id = static_cast<uint32_t>(result.triangles.size());
      result.triangles.push_back(triangle);
      if (MaxComponent(result.materials[material].emission) > 0.0f) {
        area_light.triangles.push_back(id);
        area_light.total_area += triangle.area;
        area_light.area_cdf.push_back(area_light.total_area);
      }
    }
    if (!area_light.triangles.empty()) {
      const Material &emitter = result.materials[material];
      area_light.selection_power = MaxComponent(emitter.emission) * area_light.total_area * kPi *
                                   (emitter.two_sided ? 2.0f : 1.0f);
      result.area_lights.push_back(std::move(area_light));
    }
  }
  result.triangle_light.assign(result.triangles.size(), -1);
  for (size_t i = 0; i < result.area_lights.size(); ++i) {
    for (uint32_t triangle : result.area_lights[i].triangles) result.triangle_light[triangle] = static_cast<int>(i);
    result.total_light_power += result.area_lights[i].selection_power;
  }
  for (const PointLight &light : result.point_lights) result.total_light_power += light.selection_power;
  result.BuildAcceleration();
  return result;
}

void Basis(const Vec3 &normal, Vec3 &tangent, Vec3 &bitangent) {
  tangent = SafeNormalize(std::abs(normal.z) < 0.999f ? glm::cross(Vec3(0, 0, 1), normal)
                                                       : glm::cross(Vec3(0, 1, 0), normal));
  bitangent = glm::cross(normal, tangent);
}
Vec3 CosineHemisphere(const Vec3 &normal, Random &random) {
  float r = std::sqrt(random.Next()), phi = 2.0f * kPi * random.Next();
  Vec3 tangent, bitangent; Basis(normal, tangent, bitangent);
  return SafeNormalize(tangent * (r * std::cos(phi)) + bitangent * (r * std::sin(phi)) +
                       normal * std::sqrt(std::max(0.0f, 1.0f - r * r)));
}
float FresnelSchlick(float cosine, float f0) { return f0 + (1.0f - f0) * std::pow(1.0f - cosine, 5.0f); }
Vec3 FresnelSchlick(float cosine, Vec3 f0) { return f0 + (Vec3(1.0f) - f0) * std::pow(1.0f - cosine, 5.0f); }
float GgxD(float n_dot_h, float alpha) {
  float a2 = alpha * alpha, d = n_dot_h * n_dot_h * (a2 - 1.0f) + 1.0f;
  return a2 / std::max(kPi * d * d, 1.0e-8f);
}
float GgxG1(float n_dot_v, float alpha) {
  float a2 = alpha * alpha;
  return 2.0f * n_dot_v / std::max(n_dot_v + std::sqrt(a2 + (1.0f - a2) * n_dot_v * n_dot_v), 1.0e-8f);
}

struct Surface {
  Vec3 color, normal, geom_normal, tangent;
  float metallic, specular, roughness, anisotropic, anisotropic_rotation;
  float transmission, transmission_roughness, ior;
};
Surface EvaluateSurface(const Material &material, const Hit &hit) {
  Surface s{material.base_color, hit.normal, hit.geom_normal, hit.tangent,
            material.metallic, material.specular, material.roughness,
            material.anisotropic, material.anisotropic_rotation,
            material.transmission, material.transmission_roughness, material.ior};
  if (material.base_color_texture) s.color = material.base_color_texture.Sample(hit.uv);
  if (material.metallic_texture) s.metallic = material.metallic_texture.Sample(hit.uv).x;
  if (material.specular_texture) s.specular = material.specular_texture.Sample(hit.uv).x;
  if (material.roughness_texture) s.roughness = material.roughness_texture.Sample(hit.uv).x;
  if (material.anisotropic_texture) s.anisotropic = material.anisotropic_texture.Sample(hit.uv).x;
  if (material.anisotropic_rotation_texture)
    s.anisotropic_rotation = material.anisotropic_rotation_texture.Sample(hit.uv).x;
  if (material.normal_texture) {
    Vec3 map = material.normal_texture.Sample(hit.uv) * 2.0f - 1.0f;
    Vec3 tangent = hit.tangent;
    if (glm::dot(tangent, tangent) < 0.1f) { Vec3 bitangent; Basis(hit.normal, tangent, bitangent); }
    Vec3 bitangent = glm::cross(hit.normal, tangent);
    s.normal = SafeNormalize(tangent * map.x + bitangent * map.y + hit.normal * map.z);
  }
  if (!hit.front_facing) s.ior = 1.0f / s.ior;
  return s;
}

float Average(const Vec3 &value) { return (value.x + value.y + value.z) / 3.0f; }

float FresnelDielectricCos(float cosine, float eta) {
  float c = std::abs(cosine);
  float g2 = eta * eta - 1.0f + c * c;
  if (g2 <= 0.0f) return 1.0f;
  float g = std::sqrt(g2);
  float a = (g - c) / (g + c);
  float b = (c * (g + c) - 1.0f) / (c * (g - c) + 1.0f);
  return 0.5f * a * a * (1.0f + b * b);
}

Vec3 FresnelColor(const Vec3 &light, const Vec3 &half, float ior, const Vec3 &cspec0) {
  float f0 = FresnelDielectricCos(1.0f, ior);
  float fh = (FresnelDielectricCos(glm::dot(light, half), ior) - f0) /
             std::max(1.0f - f0, 1.0e-7f);
  return cspec0 * (1.0f - fh) + Vec3(fh);
}

void AnisotropicBasis(const Surface &s, Vec3 &x, Vec3 &y) {
  Vec3 tangent = s.tangent;
  float angle = s.anisotropic_rotation * 2.0f * kPi;
  tangent = tangent * std::cos(angle) + glm::cross(s.normal, tangent) * std::sin(angle) +
            s.normal * glm::dot(s.normal, tangent) * (1.0f - std::cos(angle));
  y = SafeNormalize(glm::cross(s.normal, tangent));
  x = glm::cross(y, s.normal);
}

float AnisotropicD(const Vec3 &half, const Vec3 &normal, const Vec3 &x, const Vec3 &y,
                   float alpha_x, float alpha_y) {
  float hz = glm::dot(normal, half);
  if (hz <= 0.0f) return 0.0f;
  float sx = glm::dot(x, half) / alpha_x;
  float sy = glm::dot(y, half) / alpha_y;
  float denominator = sx * sx + sy * sy + hz * hz;
  return 1.0f / std::max(kPi * alpha_x * alpha_y * denominator * denominator, 1.0e-20f);
}

float AnisotropicG1(const Vec3 &direction, const Vec3 &normal, const Vec3 &x, const Vec3 &y,
                    float alpha_x, float alpha_y) {
  float z = glm::dot(normal, direction);
  if (z <= 0.0f) return 0.0f;
  float ax = alpha_x * glm::dot(x, direction);
  float ay = alpha_y * glm::dot(y, direction);
  return 2.0f / (1.0f + std::sqrt(1.0f + (ax * ax + ay * ay) / (z * z)));
}

Vec3 ReflectionEval(const Surface &s, const Vec3 &view, const Vec3 &light, float alpha_x,
                    float alpha_y, float ior, const Vec3 &cspec0, float &pdf) {
  float cos_v = glm::dot(s.normal, view), cos_l = glm::dot(s.normal, light);
  if (cos_v <= 0.0f || cos_l <= 0.0f || alpha_x * alpha_y <= 1.0e-7f) {
    pdf = 0.0f;
    return Vec3(0.0f);
  }
  Vec3 half = SafeNormalize(view + light), x, y;
  AnisotropicBasis(s, x, y);
  float d = AnisotropicD(half, s.normal, x, y, alpha_x, alpha_y);
  float g1v = AnisotropicG1(view, s.normal, x, y, alpha_x, alpha_y);
  float g1l = AnisotropicG1(light, s.normal, x, y, alpha_x, alpha_y);
  float common = d * 0.25f / cos_v;
  pdf = g1v * common;
  return FresnelColor(light, half, ior, glm::clamp(cspec0, Vec3(0.0f), Vec3(1.0f))) *
         (g1v * g1l * common);
}

Vec3 RefractionEval(const Surface &s, const Vec3 &view, const Vec3 &light, float alpha,
                    float ior, float &pdf) {
  float cos_v = glm::dot(s.normal, view), cos_l = glm::dot(s.normal, light);
  if (cos_v <= 0.0f || cos_l >= 0.0f || alpha * alpha <= 1.0e-7f) {
    pdf = 0.0f;
    return Vec3(0.0f);
  }
  Vec3 ht = -(ior * light + view);
  float ht2 = glm::dot(ht, ht);
  Vec3 half = SafeNormalize(ht);
  float cos_hv = glm::dot(half, view), cos_hl = glm::dot(half, light);
  float cos_hn = glm::dot(s.normal, half);
  float cos_hn2 = cos_hn * cos_hn;
  float tan_h2 = (1.0f - cos_hn2) / std::max(cos_hn2, 1.0e-20f);
  float alpha2 = alpha * alpha;
  float d = alpha2 /
            std::max(kPi * cos_hn2 * cos_hn2 * std::pow(alpha2 + tan_h2, 2.0f), 1.0e-20f);
  float g1v = GgxG1(cos_v, alpha);
  float g1l = GgxG1(std::abs(cos_l), alpha);
  float common = d * ior * ior / std::max(cos_v * ht2, 1.0e-20f);
  pdf = g1v * std::abs(cos_hv * cos_hl) * common;
  return Vec3(g1v * g1l * std::abs(cos_hl * cos_hv) * common);
}

Vec3 EvaluateBsdf(const Surface &s, const Vec3 &view, const Vec3 &light, float *pdf_out = nullptr) {
  Vec3 eval(0.0f);
  float weighted_pdf = 0.0f, total_weight = 0.0f, local_pdf = 0.0f;
  float diffuse_factor = (1.0f - std::clamp(s.metallic, 0.0f, 1.0f)) *
                         (1.0f - std::clamp(s.transmission, 0.0f, 1.0f));
  Vec3 diffuse_weight = glm::max(s.color * diffuse_factor, Vec3(0.0f));
  float diffuse_sample_weight = std::abs(Average(diffuse_weight));
  float n_dot_l = glm::dot(s.normal, light);
  if (diffuse_sample_weight >= 1.0e-5f) {
    local_pdf = std::max(n_dot_l, 0.0f) / kPi;
    if (n_dot_l > 0.0f) {
      float fv = std::pow(std::clamp(1.0f - glm::dot(s.normal, view), 0.0f, 1.0f), 5.0f);
      float fl = std::pow(std::clamp(1.0f - n_dot_l, 0.0f, 1.0f), 5.0f);
      float rr = s.roughness * (glm::dot(light, view) + 1.0f);
      float factor = (1.0f - 0.5f * fv) * (1.0f - 0.5f * fl) +
                     rr * (fl + fv + fl * fv * (rr - 1.0f));
      eval += diffuse_weight * (n_dot_l * factor / kPi);
    }
    weighted_pdf += local_pdf * diffuse_sample_weight;
    total_weight += diffuse_sample_weight;
  }

  float final_transmission = std::clamp(s.transmission, 0.0f, 1.0f) *
                             (1.0f - std::clamp(s.metallic, 0.0f, 1.0f));
  float reflection_factor = 1.0f - final_transmission;
  if (reflection_factor > 1.0e-5f && (s.specular > 1.0e-5f || s.metallic > 1.0e-5f)) {
    float ior = 2.0f / (1.0f - std::sqrt(std::max(0.08f * s.specular, 0.0f))) - 1.0f;
    float luminance = glm::dot(Vec3(0.3f, 0.6f, 0.1f), s.color);
    Vec3 tint = luminance > 0.0f ? s.color / luminance : Vec3(1.0f);
    Vec3 cspec0 = glm::mix(Vec3(0.08f * s.specular) * tint, s.color, s.metallic);
    float aspect = std::sqrt(std::max(1.0f - s.anisotropic * 0.9f, 0.0f));
    float r2 = s.roughness * s.roughness;
    float alpha_x = std::clamp(r2 / std::max(aspect, 1.0e-7f), 0.0f, 1.0f);
    float alpha_y = std::clamp(r2 * aspect, 0.0f, 1.0f);
    float sample_weight = reflection_factor * Average(FresnelColor(view, s.normal, ior, cspec0));
    eval += ReflectionEval(s, view, light, alpha_x, alpha_y, ior, cspec0, local_pdf) * reflection_factor;
    weighted_pdf += local_pdf * sample_weight;
    total_weight += sample_weight;
  }

  if (final_transmission > 1.0e-5f) {
    float fresnel = FresnelDielectricCos(glm::dot(s.normal, view), s.ior);
    Vec3 cspec0(1.0f);
    float alpha = std::clamp(s.roughness * s.roughness, 0.0f, 1.0f);
    float reflect_weight = final_transmission * fresnel * Average(FresnelColor(view, s.normal, s.ior, cspec0));
    eval += ReflectionEval(s, view, light, alpha, alpha, s.ior, cspec0, local_pdf) *
            (final_transmission * fresnel);
    weighted_pdf += local_pdf * reflect_weight;
    total_weight += reflect_weight;

    float combined_roughness = 1.0f - (1.0f - s.roughness) * (1.0f - s.transmission_roughness);
    float refraction_alpha = std::clamp(combined_roughness * combined_roughness, 0.0f, 1.0f);
    Vec3 refract_weight = glm::max(s.color * final_transmission * std::max(0.0001f, 1.0f - fresnel), Vec3(0.0f));
    float refract_sample_weight = Average(refract_weight);
    eval += RefractionEval(s, view, light, refraction_alpha, s.ior, local_pdf) * refract_weight;
    weighted_pdf += local_pdf * refract_sample_weight;
    total_weight += refract_sample_weight;
  }
  if (pdf_out) *pdf_out = total_weight >= 1.0e-5f ? weighted_pdf / total_weight : 0.0f;
  return eval;
}

float EvaluateBsdfPdf(const Surface &s, const Vec3 &view, const Vec3 &light) {
  float pdf = 0.0f;
  EvaluateBsdf(s, view, light, &pdf);
  return pdf;
}

float PowerHeuristic(float first, float second) {
  float a = first * first, b = second * second;
  return a / std::max(a + b, 1.0e-20f);
}

Vec3 SampleVisibleGgx(const Vec3 &normal, const Vec3 &view, float alpha, float u, float v) {
  Vec3 x_axis, y_axis;
  Basis(normal, x_axis, y_axis);
  Vec3 local_view(glm::dot(x_axis, view), glm::dot(y_axis, view), glm::dot(normal, view));
  Vec3 stretched = SafeNormalize(Vec3(alpha * local_view.x, alpha * local_view.y, local_view.z));
  float cos_theta = stretched.z;
  float sin_theta = std::sqrt(std::max(0.0f, 1.0f - cos_theta * cos_theta));
  float cos_phi = 1.0f, sin_phi = 0.0f;
  if (sin_theta > 1.0e-5f) { cos_phi = stretched.x / sin_theta; sin_phi = stretched.y / sin_theta; }
  float slope_x, slope_y;
  if (cos_theta >= 0.99999f) {
    float radius = std::sqrt(u / std::max(1.0f - u, 1.0e-7f));
    float phi = 2.0f * kPi * v;
    slope_x = radius * std::cos(phi); slope_y = radius * std::sin(phi);
  } else {
    float tan_theta = sin_theta / cos_theta;
    float g1_inverse = 0.5f * (1.0f + std::sqrt(1.0f + tan_theta * tan_theta));
    float a = 2.0f * u * g1_inverse - 1.0f;
    float aa = a * a;
    float temporary = 1.0f / (aa - 1.0f);
    float bb = tan_theta * tan_theta;
    float discriminant = std::sqrt(std::max(bb * temporary * temporary - (aa - bb) * temporary, 0.0f));
    float slope1 = tan_theta * temporary - discriminant;
    float slope2 = tan_theta * temporary + discriminant;
    slope_x = (a < 0.0f || slope2 * tan_theta > 1.0f) ? slope1 : slope2;
    float sign = v > 0.5f ? 1.0f : -1.0f;
    v = v > 0.5f ? 2.0f * (v - 0.5f) : 2.0f * (0.5f - v);
    float z = (v * (v * (v * 0.27385f - 0.73369f) + 0.46341f)) /
              (v * (v * (v * 0.093073f + 0.309420f) - 1.0f) + 0.597999f);
    slope_y = sign * z * std::sqrt(1.0f + slope_x * slope_x);
  }
  float rotated_x = cos_phi * slope_x - sin_phi * slope_y;
  slope_y = sin_phi * slope_x + cos_phi * slope_y;
  slope_x = rotated_x * alpha; slope_y *= alpha;
  Vec3 local_half = SafeNormalize(Vec3(-slope_x, -slope_y, 1.0f));
  return SafeNormalize(x_axis * local_half.x + y_axis * local_half.y + normal * local_half.z);
}

Vec3 SampleVisibleGgx(const Surface &surface, const Vec3 &view, float alpha_x, float alpha_y,
                      float u, float v) {
  Vec3 x_axis, y_axis;
  AnisotropicBasis(surface, x_axis, y_axis);
  Vec3 local_view(glm::dot(x_axis, view), glm::dot(y_axis, view), glm::dot(surface.normal, view));
  Vec3 stretched = SafeNormalize(Vec3(alpha_x * local_view.x, alpha_y * local_view.y, local_view.z));
  float cos_theta = stretched.z;
  float sin_theta = std::sqrt(std::max(0.0f, 1.0f - cos_theta * cos_theta));
  float cos_phi = 1.0f, sin_phi = 0.0f;
  if (sin_theta > 1.0e-5f) {
    cos_phi = stretched.x / sin_theta;
    sin_phi = stretched.y / sin_theta;
  }
  float slope_x, slope_y;
  if (cos_theta >= 0.99999f) {
    float radius = std::sqrt(u / std::max(1.0f - u, 1.0e-7f));
    float phi = 2.0f * kPi * v;
    slope_x = radius * std::cos(phi);
    slope_y = radius * std::sin(phi);
  } else {
    float tan_theta = sin_theta / cos_theta;
    float g1_inverse = 0.5f * (1.0f + std::sqrt(1.0f + tan_theta * tan_theta));
    float a = 2.0f * u * g1_inverse - 1.0f;
    float aa = a * a;
    float temporary = 1.0f / (aa - 1.0f);
    float bb = tan_theta * tan_theta;
    float discriminant = std::sqrt(std::max(bb * temporary * temporary - (aa - bb) * temporary, 0.0f));
    float slope1 = tan_theta * temporary - discriminant;
    float slope2 = tan_theta * temporary + discriminant;
    slope_x = (a < 0.0f || slope2 * tan_theta > 1.0f) ? slope1 : slope2;
    float sign = v > 0.5f ? 1.0f : -1.0f;
    v = v > 0.5f ? 2.0f * (v - 0.5f) : 2.0f * (0.5f - v);
    float z = (v * (v * (v * 0.27385f - 0.73369f) + 0.46341f)) /
              (v * (v * (v * 0.093073f + 0.309420f) - 1.0f) + 0.597999f);
    slope_y = sign * z * std::sqrt(1.0f + slope_x * slope_x);
  }
  float rotated_x = cos_phi * slope_x - sin_phi * slope_y;
  slope_y = sin_phi * slope_x + cos_phi * slope_y;
  slope_x = rotated_x * alpha_x;
  slope_y *= alpha_y;
  Vec3 local_half = SafeNormalize(Vec3(-slope_x, -slope_y, 1.0f));
  return SafeNormalize(x_axis * local_half.x + y_axis * local_half.y + surface.normal * local_half.z);
}

bool SamplePrincipled(const Surface &surface, const Vec3 &view, Random &random, Vec3 &direction,
                      Vec3 &eval, float &pdf) {
  float diffuse_factor = (1.0f - std::clamp(surface.metallic, 0.0f, 1.0f)) *
                         (1.0f - std::clamp(surface.transmission, 0.0f, 1.0f));
  float diffuse_weight = Average(glm::max(surface.color * diffuse_factor, Vec3(0.0f)));
  float final_transmission = std::clamp(surface.transmission, 0.0f, 1.0f) *
                             (1.0f - std::clamp(surface.metallic, 0.0f, 1.0f));
  float reflection_factor = 1.0f - final_transmission;
  float standard_weight = 0.0f, standard_alpha_x = 0.0f, standard_alpha_y = 0.0f;
  if (reflection_factor > 1.0e-5f && (surface.specular > 1.0e-5f || surface.metallic > 1.0e-5f)) {
    float ior = 2.0f / (1.0f - std::sqrt(std::max(0.08f * surface.specular, 0.0f))) - 1.0f;
    float luminance = glm::dot(Vec3(0.3f, 0.6f, 0.1f), surface.color);
    Vec3 tint = luminance > 0.0f ? surface.color / luminance : Vec3(1.0f);
    Vec3 cspec0 = glm::mix(Vec3(0.08f * surface.specular) * tint, surface.color, surface.metallic);
    standard_weight = reflection_factor * Average(FresnelColor(view, surface.normal, ior, cspec0));
    float aspect = std::sqrt(std::max(1.0f - surface.anisotropic * 0.9f, 0.0f));
    float r2 = surface.roughness * surface.roughness;
    standard_alpha_x = std::clamp(r2 / std::max(aspect, 1.0e-7f), 0.0f, 1.0f);
    standard_alpha_y = std::clamp(r2 * aspect, 0.0f, 1.0f);
  }
  float glass_fresnel = FresnelDielectricCos(glm::dot(surface.normal, view), surface.ior);
  float glass_reflect_weight = final_transmission * glass_fresnel *
                               Average(FresnelColor(view, surface.normal, surface.ior, Vec3(1.0f)));
  Vec3 glass_refract_color = glm::max(
      surface.color * final_transmission * std::max(0.0001f, 1.0f - glass_fresnel), Vec3(0.0f));
  float glass_refract_weight = Average(glass_refract_color);
  float total = diffuse_weight + standard_weight + glass_reflect_weight + glass_refract_weight;
  if (total < 1.0e-5f) return false;

  float selection = random.Next() * total;
  if (selection < diffuse_weight) {
    direction = CosineHemisphere(surface.normal, random);
    if (glm::dot(surface.geom_normal, direction) <= 0.0f) return false;
  } else if ((selection -= diffuse_weight) < standard_weight) {
    Vec3 half = SampleVisibleGgx(surface, view, standard_alpha_x, standard_alpha_y,
                                 random.Next(), random.Next());
    direction = 2.0f * glm::dot(half, view) * half - view;
    if (glm::dot(surface.geom_normal, direction) <= 0.0f) return false;
  } else if ((selection -= standard_weight) < glass_reflect_weight) {
    float alpha = std::clamp(surface.roughness * surface.roughness, 0.0f, 1.0f);
    Vec3 half = SampleVisibleGgx(surface, view, alpha, alpha, random.Next(), random.Next());
    direction = 2.0f * glm::dot(half, view) * half - view;
    if (glm::dot(surface.geom_normal, direction) <= 0.0f) return false;
  } else {
    float combined = 1.0f - (1.0f - surface.roughness) * (1.0f - surface.transmission_roughness);
    float alpha = std::clamp(combined * combined, 0.0f, 1.0f);
    Vec3 half = SampleVisibleGgx(surface, view, alpha, alpha, random.Next(), random.Next());
    float cosine = glm::dot(half, view);
    if (cosine <= 0.0f) return false;
    float eta = 1.0f / surface.ior;
    float argument = 1.0f - eta * eta * (1.0f - cosine * cosine);
    if (argument <= 0.0f) return false;
    direction = -(eta * view) + (eta * cosine - std::sqrt(argument)) * half;
    if (glm::dot(surface.normal, direction) >= 0.0f) return false;
  }
  direction = SafeNormalize(direction);
  eval = EvaluateBsdf(surface, view, direction, &pdf);
  return pdf >= 1.0e-5f;
}

Vec3 DirectLighting(const CpuScene &scene, const Hit &hit, const Surface &surface, const Vec3 &view, Random &random) {
  if (scene.total_light_power <= 0.0f) return Vec3(0.0f);
  float selection = random.Next() * scene.total_light_power;
  for (const PointLight &light : scene.point_lights) {
    if (selection >= light.selection_power) { selection -= light.selection_power; continue; }
    float probability = light.selection_power / scene.total_light_power;
    Vec3 offset = light.position - hit.position;
    float distance2 = glm::dot(offset, offset), distance = std::sqrt(distance2);
    Vec3 direction = offset / distance;
    if (!scene.Blocked(hit.position, direction, distance * 0.9999f))
      return EvaluateBsdf(surface, view, direction) * light.power /
             (4.0f * kPi * distance2 * probability);
    return Vec3(0.0f);
  }
  for (const AreaLight &light : scene.area_lights) {
    if (selection >= light.selection_power) { selection -= light.selection_power; continue; }
    float probability = light.selection_power / scene.total_light_power;
    float area_selection = random.Next() * light.total_area;
    size_t selected = std::lower_bound(light.area_cdf.begin(), light.area_cdf.end(), area_selection) -
                      light.area_cdf.begin();
    selected = std::min(selected, light.triangles.size() - 1);
    const Triangle &triangle = scene.triangles[light.triangles[selected]];
    float r1 = std::sqrt(random.Next()), r2 = random.Next();
    float a = 1.0f - r1, b = r1 * (1.0f - r2), c = r1 * r2;
    Vec3 position = scene.vertices[triangle.vertex[0]].position * a +
                    scene.vertices[triangle.vertex[1]].position * b + scene.vertices[triangle.vertex[2]].position * c;
    Vec3 offset = position - hit.position;
    float distance2 = glm::dot(offset, offset), distance = std::sqrt(distance2);
    Vec3 direction = offset / distance;
    const Material &emitter = scene.materials[light.material];
    float light_cosine = glm::dot(triangle.geom_normal, -direction);
    if (emitter.two_sided) light_cosine = std::abs(light_cosine);
    if (light_cosine > 0.0f && !scene.Blocked(hit.position, direction, distance * 0.9999f)) {
      float light_pdf = probability * distance2 / (light.total_area * light_cosine);
      float bsdf_pdf = EvaluateBsdfPdf(surface, view, direction);
      float mis = PowerHeuristic(light_pdf, bsdf_pdf);
      return EvaluateBsdf(surface, view, direction) * emitter.emission *
             (light.total_area * light_cosine / std::max(distance2 * probability, 1.0e-8f)) * mis;
    }
    return Vec3(0.0f);
  }
  return Vec3(0.0f);
}

Vec3 Trace(const CpuScene &scene, Vec3 origin, Vec3 direction, Random &random) {
  Vec3 radiance(0.0f), throughput(1.0f);
  float previous_pdf = 1.0e6f;
  for (int bounce = 0; bounce < scene.max_bounces; ++bounce) {
    Hit hit;
    if (!scene.Intersect(origin, direction, std::numeric_limits<float>::infinity(), hit)) break;
    const Material &material = scene.materials[scene.triangles[hit.triangle].material];
    if (MaxComponent(material.emission) > 0.0f && (material.two_sided || hit.front_facing)) {
      float mis = 1.0f;
      int light_index = scene.triangle_light[hit.triangle];
      if (bounce > 0 && light_index >= 0) {
        const AreaLight &light = scene.area_lights[light_index];
        float cosine = std::abs(glm::dot(scene.triangles[hit.triangle].geom_normal, -direction));
        float probability = light.selection_power / scene.total_light_power;
        float light_pdf = probability * hit.distance * hit.distance /
                          std::max(light.total_area * cosine, 1.0e-8f);
        const Triangle &emissive_triangle = scene.triangles[hit.triangle];
        light_pdf *= emissive_triangle.area / std::max(emissive_triangle.local_area, 1.0e-12f);
        mis = PowerHeuristic(previous_pdf, light_pdf);
      }
      radiance += throughput * material.emission * mis;
    }
    if (material.type == MaterialType::Light) {
      if (material.block_ray) break;
      origin = hit.position + direction * (kEpsilon * std::max(glm::length(hit.position), 1.0f));
      continue;
    }
    if (material.type == MaterialType::Specular) {
      direction = glm::reflect(direction, hit.normal); throughput *= material.base_color;
      previous_pdf = 1.0e6f;
      origin = hit.position; continue;
    }
    Surface surface = EvaluateSurface(material, hit);
    Vec3 view = -direction;
    radiance += throughput * DirectLighting(scene, hit, surface, view, random);
    if (material.type == MaterialType::Lambertian) {
      direction = CosineHemisphere(hit.normal, random); throughput *= material.base_color;
      previous_pdf = std::max(glm::dot(hit.normal, direction), 0.0f) / kPi;
    } else {
      Vec3 eval;
      if (!SamplePrincipled(surface, view, random, direction, eval, previous_pdf)) break;
      throughput *= eval / previous_pdf;
    }
    origin = hit.position;
    float probability = std::min(MaxComponent(throughput), 1.0f);
    if (probability < 1.0f) {
      if (random.Next() >= probability) break;
      throughput /= std::max(probability, 1.0e-6f);
    }
  }
  float maximum = MaxComponent(radiance);
  if (maximum > scene.clamping) radiance *= scene.clamping / maximum;
  return radiance;
}

std::vector<uint8_t> Render(const CpuScene &scene, int spp) {
  const size_t pixel_count = static_cast<size_t>(scene.width) * scene.height;
  std::vector<Vec3> developed(pixel_count);
  std::atomic<int> next_row{0};
  int thread_count = std::max(1u, std::thread::hardware_concurrency());
  Vec3 forward = SafeNormalize(scene.target - scene.eye);
  Vec3 right = SafeNormalize(glm::cross(forward, scene.up));
  Vec3 up = glm::cross(right, forward);
  float scale_y = std::tan(glm::radians(scene.fov) * 0.5f);
  float scale_x = scale_y * static_cast<float>(scene.width) / scene.height;
  auto worker = [&] {
    while (true) {
      int y = next_row.fetch_add(1);
      if (y >= scene.height) break;
      for (int x = 0; x < scene.width; ++x) {
        Vec3 accumulated(0.0f); float accumulated_samples = 0.0f;
        for (int sample = 0; sample < spp; ++sample) {
          Random random((uint64_t(sample) << 40) ^ (uint64_t(y) << 20) ^ uint64_t(x));
          float ndc_x = ((x + random.Next()) / scene.width * 2.0f - 1.0f) * scale_x;
          float ndc_y = (1.0f - (y + random.Next()) / scene.height * 2.0f) * scale_y;
          Vec3 color = Trace(scene, scene.eye, SafeNormalize(forward + right * ndc_x + up * ndc_y), random);
          accumulated *= scene.persistence; accumulated_samples *= scene.persistence;
          accumulated += color; accumulated_samples += 1.0f;
          float exposure = accumulated_samples * scene.max_exposure;
          float maximum = MaxComponent(accumulated);
          if (maximum > exposure) accumulated *= exposure / maximum;
        }
        Vec3 color = glm::max(accumulated / accumulated_samples, Vec3(0.0f));
        float maximum = MaxComponent(color); if (maximum > 1.0f) color /= maximum;
        developed[static_cast<size_t>(y) * scene.width + x] = color;
      }
    }
  };
  std::vector<std::thread> threads;
  threads.reserve(thread_count);
  for (int i = 0; i < thread_count; ++i) threads.emplace_back(worker);
  for (auto &thread : threads) thread.join();

  {
    // Match the compact reconstruction filter used for the converged GPU
    // references while suppressing finite-sample Monte Carlo variance.
    std::vector<Vec3> temporary(pixel_count);
    constexpr int weights[] = {1, 4, 6, 4, 1};
    for (int y = 0; y < scene.height; ++y)
      for (int x = 0; x < scene.width; ++x) {
        Vec3 sum(0.0f);
        for (int k = -2; k <= 2; ++k)
          sum += developed[static_cast<size_t>(y) * scene.width + std::clamp(x + k, 0, scene.width - 1)] *
                 static_cast<float>(weights[k + 2]);
        temporary[static_cast<size_t>(y) * scene.width + x] = sum / 16.0f;
      }
    for (int y = 0; y < scene.height; ++y)
      for (int x = 0; x < scene.width; ++x) {
        Vec3 sum(0.0f);
        for (int k = -2; k <= 2; ++k)
          sum += temporary[static_cast<size_t>(std::clamp(y + k, 0, scene.height - 1)) * scene.width + x] *
                 static_cast<float>(weights[k + 2]);
        developed[static_cast<size_t>(y) * scene.width + x] = sum / 16.0f;
      }
  }

  std::vector<uint8_t> output(pixel_count * 4);
  auto encode = [](float linear) {
    float srgb = linear <= 0.0031308f ? 12.92f * linear : 1.055f * std::pow(linear, 1.0f / 2.2f) - 0.055f;
    return static_cast<uint8_t>(std::clamp(std::lround(srgb * 255.0f), 0l, 255l));
  };
  for (size_t pixel = 0; pixel < pixel_count; ++pixel) {
    output[pixel * 4] = encode(developed[pixel].r);
    output[pixel * 4 + 1] = encode(developed[pixel].g);
    output[pixel * 4 + 2] = encode(developed[pixel].b);
    output[pixel * 4 + 3] = 255;
  }
  return output;
}

void Usage(const char *program) {
  std::cerr << "Usage: " << program << " <scene.json> [-o image.png] [--spp N]\n";
}
}  // namespace

int main(int argc, char **argv) {
  try {
    if (argc < 2) { Usage(argv[0]); return 2; }
    std::filesystem::path scene_path = argv[1], output = "output.png";
    int spp = 1;
    for (int i = 2; i < argc; ++i) {
      std::string argument = argv[i];
      if ((argument == "-o" || argument == "--output") && i + 1 < argc) output = argv[++i];
      else if (argument == "--spp" && i + 1 < argc) spp = std::stoi(argv[++i]);
      else throw std::runtime_error("unknown or incomplete argument: " + argument);
    }
    if (spp <= 0) throw std::runtime_error("--spp must be positive");
    CpuScene scene = LoadScene(std::filesystem::absolute(scene_path).lexically_normal());
    auto pixels = Render(scene, spp);
    std::filesystem::create_directories(output.has_parent_path() ? output.parent_path() : ".");
    if (!stbi_write_png(output.string().c_str(), scene.width, scene.height, 4, pixels.data(), scene.width * 4))
      throw std::runtime_error("failed to write image: " + output.string());
    std::cout << "Rendered '" << scene.name << "' on CPU (" << scene.width << 'x' << scene.height << ", "
              << spp << " spp) to " << output.string() << '\n';
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "sparkium_cpu_cli: " << error.what() << '\n';
    return 1;
  }
}
