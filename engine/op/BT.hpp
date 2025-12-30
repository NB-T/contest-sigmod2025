#pragma once
// Header so that everything is backward compatible. BT is just an alias for Hashtable.
#include "op/Hashtable.hpp"
namespace engine {
using BT = Hashtable;
using BTBuild = HashtableBuild;
using BTProbe = HashtableProbe;
}