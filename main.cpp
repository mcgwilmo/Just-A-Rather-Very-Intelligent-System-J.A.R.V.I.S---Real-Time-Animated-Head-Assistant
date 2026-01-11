#include <iostream>
#include <chrono>

#include "HeadViewerApp.hpp"

using namespace GLOO;

int main(int argc, char** argv) {
    // Default mesh path for convenience; can be overridden via command line.
    const std::string kDefaultMeshPath = "head_variants/head3/mesh/head.obj";

    std::string mesh_path;
    if (argc < 2) {
        std::cout << "Usage: " << argv[0] << " [HEAD_MESH_PATH]\n"
                  << "  If HEAD_MESH_PATH is not provided, the default is:\n"
                  << "    " << kDefaultMeshPath << "\n\n"
                  << "Examples:\n"
                  << "  " << argv[0] << " " << kDefaultMeshPath << "\n"
                  << "  " << argv[0] << "head_variants/head2/mesh/head.obj\n"
                  << std::endl;
        mesh_path = kDefaultMeshPath;
    } else {
        mesh_path = argv[1];
    }

    std::unique_ptr<HeadViewerApp> app =
        make_unique<HeadViewerApp>("Final Project - Head Viewer",
                                   glm::ivec2(1440, 900),
                                   mesh_path);

    app->SetupScene();

    using Clock = std::chrono::high_resolution_clock;
    using TimePoint =
        std::chrono::time_point<Clock, std::chrono::duration<double>>;
    TimePoint last_tick_time = Clock::now();
    TimePoint start_tick_time = last_tick_time;

    while (!app->IsFinished()) {
        TimePoint current_tick_time = Clock::now();
        double delta_time = (current_tick_time - last_tick_time).count();
        double total_elapsed_time =
            (current_tick_time - start_tick_time).count();
        app->Tick(delta_time, total_elapsed_time);
        last_tick_time = current_tick_time;
    }

    return 0;
}
