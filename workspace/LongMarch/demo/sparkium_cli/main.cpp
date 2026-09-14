#include <long_march.h>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

#include <algorithm>
#include <filesystem>
#include <iostream>

using namespace long_march;

namespace {
void Usage(const char *program) {
  std::cerr << "Usage: " << program << " <scene.json> [-o image.png] [--spp N] "
            << "[--pipeline auto|ray_tracing]\n"
            << "       " << program << " --list [scene-directory]\n";
}

sparkium::RenderPipeline ParsePipeline(const std::string &name) {
  if (name == "auto") return sparkium::RENDER_PIPELINE_AUTO;
  if (name == "ray_tracing") return sparkium::RENDER_PIPELINE_RAY_TRACING;
  throw std::runtime_error("unknown pipeline: " + name);
}
}  // namespace

int main(int argc, char **argv) {
  try {
    if (argc < 2) {
      Usage(argv[0]);
      return 2;
    }
    if (std::string(argv[1]) == "--list") {
      auto directory = argc > 2 ? std::filesystem::path(argv[2])
                                : std::filesystem::path(FindAssetPath("scenes"));
      for (const auto &path : sparkium::FindJsonScenes(directory)) std::cout << path.string() << '\n';
      return 0;
    }

    std::filesystem::path scene_path = argv[1];
    std::filesystem::path output = "output.png";
    int spp = 1;
    bool override_pipeline = false;
    sparkium::RenderPipeline pipeline = sparkium::RENDER_PIPELINE_AUTO;
    for (int i = 2; i < argc; ++i) {
      std::string argument = argv[i];
      if ((argument == "-o" || argument == "--output") && i + 1 < argc) output = argv[++i];
      else if (argument == "--spp" && i + 1 < argc) spp = std::stoi(argv[++i]);
      else if (argument == "--pipeline" && i + 1 < argc) {
        pipeline = ParsePipeline(argv[++i]);
        override_pipeline = true;
      } else {
        throw std::runtime_error("unknown or incomplete argument: " + argument);
      }
    }
    if (spp <= 0) throw std::runtime_error("--spp must be positive");

    std::unique_ptr<graphics::Core> graphics_core;
    if (graphics::CreateCore(graphics::BACKEND_API_DEFAULT, graphics::Core::Settings{}, &graphics_core) != 0)
      throw std::runtime_error("failed to create graphics core");
    graphics_core->InitializeLogicalDeviceAutoSelect(false);
    sparkium::Core core(graphics_core.get());
    std::string error;
    auto loaded = sparkium::JsonScene::Load(&core, scene_path, &error);
    if (!loaded) throw std::runtime_error(error);
    if (!override_pipeline) pipeline = loaded->GetRenderPipeline();

    auto *film = loaded->GetFilm();
    std::unique_ptr<graphics::Image> image;
    graphics_core->CreateImage(film->GetWidth(), film->GetHeight(), graphics::IMAGE_FORMAT_R8G8B8A8_UNORM, &image);
    auto *scene = loaded->GetScene();
    const int samples_per_dispatch = scene->settings.samples_per_dispatch;
    for (int remaining = spp; remaining > 0; remaining -= scene->settings.samples_per_dispatch) {
      scene->settings.samples_per_dispatch = std::min(samples_per_dispatch, remaining);
      core.Render(loaded->GetScene(), loaded->GetCamera(), film, pipeline);
    }
    scene->settings.samples_per_dispatch = samples_per_dispatch;
    film->Develop(image.get());
    std::vector<uint8_t> pixels(static_cast<size_t>(film->GetWidth()) * film->GetHeight() * 4);
    image->DownloadData(pixels.data());
    std::filesystem::create_directories(output.has_parent_path() ? output.parent_path() : ".");
    if (!stbi_write_png(output.string().c_str(), film->GetWidth(), film->GetHeight(), 4, pixels.data(),
                        film->GetWidth() * 4))
      throw std::runtime_error("failed to write image: " + output.string());
    std::cout << "Rendered '" << loaded->GetName() << "' (" << film->GetWidth() << 'x' << film->GetHeight()
              << ", " << spp << " spp) to " << output.string() << '\n';
    return 0;
  } catch (const std::exception &exception) {
    std::cerr << "sparkium_cli: " << exception.what() << '\n';
    return 1;
  }
}
