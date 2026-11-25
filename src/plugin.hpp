#pragma once
#ifdef FRACTAL_HEADLESS_TEST
#include "../tests/rack_test_double.hpp"
#else
#include <rack.hpp>
#endif
using namespace rack;
extern Plugin* pluginInstance;
extern Model* modelFractalXY;
