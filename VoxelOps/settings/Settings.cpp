#include "Settings.hpp"
#include <nlohmann/json.hpp>
#include <fstream>

Settings::Data Settings::current;

namespace Settings
{
	void loadFromJson(Data& data, const std::filesystem::path& path) {
		
	}

	void saveToJson(const Data& data, const std::filesystem::path& path) {

	}

}