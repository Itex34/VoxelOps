#include "application/App.hpp"
#include "../Shared/runtime/Paths.hpp"

int main(int argc, char** argv) {
	Shared::RuntimePaths::Initialize((argc > 0 && argv != nullptr) ? argv[0] : "");

	App app;
	return app.Run(argc, argv);
}
