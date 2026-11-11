// Loads engine plugins next to the program. OBS 33 layout: core/<name>/<name>.dll with core/<name>/data;
// MyOBS own plugins: obs-plugins/64bit/<name>.dll with data/obs-plugins/<name>.
#pragma once

#include <string>
#include <vector>

namespace myobs {
// Engine plugins MyOBS uses. Others (game capture hooks, scripting, etc.) are not loaded at all.
const std::vector<std::string> &DefaultCorePlugins();
// Loads the listed OBS core plugins and every MyOBS plugin; calls obs_post_load_modules().
int LoadPlugins(const std::vector<std::string> &corePlugins = DefaultCorePlugins());
}
