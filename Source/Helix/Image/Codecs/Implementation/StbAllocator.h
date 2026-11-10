#pragma once

#include <Helix/Core/Basics/Types.h>

// The allocator of the copied stb files (StbDecoders.cpp): Helix.Core's heap, with null for blocks larger than any texture.
namespace helix::image::stb
{
    void* Allocate(usize size);
    void Free(void* memory);
    void* Reallocate(void* memory, usize size);
}
