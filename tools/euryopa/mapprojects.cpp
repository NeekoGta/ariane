#include "euryopa.h"
#include "mapprojects.h"

#include <algorithm>
#include <cctype>

#ifdef _WIN32
#include <windows.h>
#else
#include <dirent.h>
#include <sys/stat.h>
#endif

namespace MapProjects {

static const char *CURRENT_FILE = "current_map.txt";

static bool
dirExists(const std::string &path)
{
#ifdef _WIN32
	DWORD attributes = GetFileAttributesA(path.c_str());
	return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY);
#else
	struct stat st;
	return stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
#endif
}

static bool
movePath(const std::string &from, const std::string &to)
{
#ifdef _WIN32
	return MoveFileExA(from.c_str(), to.c_str(), 0) != 0;
#else
	return rename(from.c_str(), to.c_str()) == 0;
#endif
}

static std::string
join(const std::string &dir, const char *name)
{
	char path[1024];
	if(!BuildPath(path, sizeof(path), dir.c_str(), name))
		return std::string();
	return path;
}

static std::string
lower(std::string s)
{
	for(char &c : s)
		c = (char)tolower((unsigned char)c);
	return s;
}

static std::string
openMapDir(void)
{
	char root[1024];
	if(!GetGameRootDirectory(root, sizeof(root)))
		return std::string();
	return join(join(root, "modloader"), "Ariane");
}

static std::string
storeDir(void)
{
	char data[1024];
	if(!GetArianeDataDirectory(data, sizeof(data)))
		return std::string();
	return join(data, "maps");
}

bool
IsAvailable(void)
{
	char root[1024];
	return GetGameRootDirectory(root, sizeof(root)) && dirExists(join(root, "modloader")) && !storeDir().empty();
}

std::string
CurrentName(void)
{
	char path[1024];
	if(!GetArianeDataPath(path, sizeof(path), CURRENT_FILE))
		return std::string();
	FILE *f = fopen(path, "rb");
	if(f == nil)
		return std::string();
	char name[64] = "";
	if(fgets(name, sizeof(name), f) == nil)
		name[0] = '\0';
	fclose(f);
	std::string s = name;
	while(!s.empty() && (s.back() == '\n' || s.back() == '\r'))
		s.pop_back();
	return IsValidName(s.c_str()) ? s : std::string();
}

static bool
writeCurrentName(const char *name)
{
	char path[1024];
	if(!GetArianeDataPath(path, sizeof(path), CURRENT_FILE))
		return false;
	FILE *f = fopen(path, "wb");
	if(f == nil)
		return false;
	fprintf(f, "%s\n", name);
	fclose(f);
	return true;
}

bool
HasOpenMapFiles(void)
{
	std::string dir = openMapDir();
	return !dir.empty() && dirExists(dir);
}

std::vector<std::string>
StoredMaps(void)
{
	std::vector<std::string> maps;
	std::string dir = storeDir();
	if(dir.empty())
		return maps;
#ifdef _WIN32
	WIN32_FIND_DATAA data;
	HANDLE find = FindFirstFileA(join(dir, "*").c_str(), &data);
	if(find != INVALID_HANDLE_VALUE){
		do{
			if((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && IsValidName(data.cFileName))
				maps.push_back(data.cFileName);
		}while(FindNextFileA(find, &data));
		FindClose(find);
	}
#else
	DIR *d = opendir(dir.c_str());
	if(d){
		while(struct dirent *e = readdir(d))
			if(IsValidName(e->d_name) && dirExists(join(dir, e->d_name)))
				maps.push_back(e->d_name);
		closedir(d);
	}
#endif
	std::sort(maps.begin(), maps.end(), [](const std::string &a, const std::string &b) {
		return lower(a) < lower(b);
	});
	return maps;
}

bool
IsValidName(const char *name)
{
	size_t len = name ? strlen(name) : 0;
	if(len == 0 || len > 48 || name[0] == ' ' || name[len-1] == ' ')
		return false;
	for(size_t i = 0; i < len; i++){
		unsigned char c = (unsigned char)name[i];
		if(!isalnum(c) && c != ' ' && c != '-' && c != '_')
			return false;
	}
	return true;
}

static bool
isStored(const char *name)
{
	for(const std::string &map : StoredMaps())
		if(lower(map) == lower(name))
			return true;
	return false;
}

bool
RenameCurrent(const char *name, std::string &error)
{
	if(!IsValidName(name)){
		error = "Use 1 to 48 letters, digits, spaces, '-' or '_'.";
		return false;
	}
	if(isStored(name)){
		error = std::string("A stored map is already named ") + name + ".";
		return false;
	}
	if(!writeCurrentName(name)){
		error = "Could not write the name of the map.";
		return false;
	}
	return true;
}

bool
Switch(const char *unnamedAs, const char *openName, const char *newName, std::string &error)
{
	std::string open = openMapDir(), store = storeDir();
	if(open.empty() || store.empty() || !IsAvailable()){
		error = "The modloader folder was not found in the game folder.";
		return false;
	}

	std::string current = CurrentName();
	bool keepOpen = dirExists(open);
	if(keepOpen && current.empty()){
		if(!IsValidName(unnamedAs)){
			error = "Name the open map first (1 to 48 letters, digits, spaces, '-' or '_').";
			return false;
		}
		current = unnamedAs;
	}
	if(keepOpen && isStored(current.c_str())){
		error = "A stored map is already named " + current + ".";
		return false;
	}

	const char *next = openName ? openName : newName;
	if(!IsValidName(next)){
		error = "Use 1 to 48 letters, digits, spaces, '-' or '_' for the map name.";
		return false;
	}
	if(openName == nil && (isStored(newName) || lower(current) == lower(newName))){
		error = std::string("A map is already named ") + newName + ".";
		return false;
	}
	std::string source = openName ? join(store, openName) : std::string();
	if(openName && !dirExists(source)){
		error = std::string("The stored map ") + openName + " was not found.";
		return false;
	}

	std::string stored = join(store, current.c_str());
	if(keepOpen){
		if(!EnsureParentDirectoriesForPath(stored.c_str()) || !movePath(open, stored)){
			error = "Could not move modloader/Ariane to " + stored + ". Is a file of it open in another program?";
			return false;
		}
	}
	if(openName && !movePath(source, open)){
		if(keepOpen)
			movePath(stored, open);
		error = "Could not move " + source + " to modloader/Ariane.";
		return false;
	}
	writeCurrentName(next);
	return true;
}

}
