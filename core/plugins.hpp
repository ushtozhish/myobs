// Loads engine plugins next to the program. OBS 33 layout: core/<name>/<name>.dll with core/<name>/data;
// MyOBS own plugins: obs-plugins/64bit/<name>.dll with data/obs-plugins/<name>.
#pragma once

namespace myobs {
int LoadPlugins(); // returns the number of plugins loaded; also calls obs_post_load_modules()
}
