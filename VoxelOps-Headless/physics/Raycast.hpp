#pragma once

#include <glm/glm.hpp>

class Ray {
public:
	Ray(glm::vec3 rayOrigin, glm::vec3 rayDirection);
	glm::vec3 origin = glm::vec3(0);
	glm::vec3 direction = glm::vec3(0);

	//glm::vec3 intersect();
	bool hasIntersect();


	void castRay(glm::vec3 origin, glm::vec3 direction);
};