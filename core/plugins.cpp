#include "plugins.hpp"

#include <obs.h>
#include <util/platform.h>

#include <string>

namespace myobs {
namespace {

bool LoadOne(const std::string &dll, const std::string &data, const std::string &name)
{
	if (!os_file_exists(dll.c_str()))
		return false;
	obs_module_t *m = nullptr;
	const int r = obs_open_module(&m, dll.c_str(), os_file_exists(data.c_str()) ? data.c_str() : nullptr);
	if (r != MODULE_SUCCESS) {
		blog(LOG_WARNING, "plugin %s not opened (%d)", name.c_str(), r);
		return false;
	}
	if (!obs_init_module(m)) {
		blog(LOG_WARNING, "plugin %s failed to init", name.c_str());
		return false;
	}
	return true;
}

std::string Stem(const char *path)
{
	std::string f = path;
	const size_t slash = f.find_last_of("/\\");
	f = slash == std::string::npos ? f : f.substr(slash + 1);
	const size_t dot = f.find_last_of('.');
	return dot == std::string::npos ? f : f.substr(0, dot);
}

} // namespace

int LoadPlugins()
{
	char *exeDir = os_get_executable_path_ptr("");
	const std::string base = exeDir ? exeDir : "";
	bfree(exeDir);
	int loaded = 0;

	os_glob_t *glob = nullptr;
	if (os_glob((base + "../../core/*").c_str(), 0, &glob) == 0) {
		for (size_t i = 0; i < glob->gl_pathc; i++) {
			if (!glob->gl_pathv[i].directory)
				continue;
			const std::string dir = glob->gl_pathv[i].path;
			const std::string name = Stem(dir.c_str());
			if (LoadOne(dir + "/" + name + ".dll", dir + "/data", name))
				loaded++;
		}
		os_globfree(glob);
	}

	glob = nullptr;
	if (os_glob((base + "../../obs-plugins/64bit/*.dll").c_str(), 0, &glob) == 0) {
		for (size_t i = 0; i < glob->gl_pathc; i++) {
			const std::string name = Stem(glob->gl_pathv[i].path);
			if (LoadOne(glob->gl_pathv[i].path, base + "../../data/obs-plugins/" + name, name))
				loaded++;
		}
		os_globfree(glob);
	}

	obs_post_load_modules();
	blog(LOG_INFO, "plugins loaded: %d", loaded);
	return loaded;
}

} // namespace myobs
