#ifndef MAPPROJECTS_H
#define MAPPROJECTS_H

// Maps: the open map lives in modloader/Ariane (what the editor and the game
// load), the other ones are stored outside of modloader, in ariane/maps/<name>,
// so neither the game nor the editor loads them. Switching moves the folders,
// then Ariane restarts on the map opened (or on the stock map for a new one).

#include <string>
#include <vector>

namespace MapProjects {

// The modloader folder exists: the open map can be loaded.
bool IsAvailable(void);

// The name of the open map, empty if it was never named.
std::string CurrentName(void);

// modloader/Ariane holds something to keep.
bool HasOpenMapFiles(void);

std::vector<std::string> StoredMaps(void);

// Letters, digits, spaces, '-' and '_', 1 to 48 characters.
bool IsValidName(const char *name);

bool RenameCurrent(const char *name, std::string &error);

// Stores the open map (as unnamedAs if it has no name yet), then opens the
// stored map openName, or starts the new empty map newName (openName nil).
bool Switch(const char *unnamedAs, const char *openName, const char *newName, std::string &error);

}

#endif
