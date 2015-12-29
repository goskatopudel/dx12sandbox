#include "AssertionMacros.h"

#include <SDL.h>

bool handle_assertion_inner_loop(const char* condition, const char* file, int line, const char* function) {
	static struct SDL_assert_data assert_data = { 0, 0, condition, 0, 0, 0, 0 };

	const SDL_assert_state state = SDL_ReportAssertion(&assert_data,
		function,
		file,
		line);

	if (state == SDL_ASSERTION_RETRY) {
		return true;
	}
	else if (state == SDL_ASSERTION_BREAK) {
		SDL_TriggerBreakpoint();
	}

	return false;
}