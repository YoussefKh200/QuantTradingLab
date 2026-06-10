/**
 * @file core/events/Event.cpp
 *
 * Event subtypes are plain-data structs defined entirely in the header.
 * This translation unit exists so CMake has a concrete .cpp to compile
 * for the qtl_core library target, and to give a home to any future
 * out-of-line implementations (e.g. serialisation helpers).
 */

#include "core/events/Event.hpp"

namespace qtl {
// All event construction is inline in the header.
// Future: add serialise()/deserialise() for network transport here.
} // namespace qtl
