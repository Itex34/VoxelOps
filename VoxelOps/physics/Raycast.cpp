#include "Raycast.hpp"

Ray::Ray(glm::vec3 rayOrigin, glm::vec3 rayDirection) : origin(rayOrigin), direction(rayDirection) {

}


void Ray::castRay(glm::vec3 origin, glm::vec3 direction) {

}

bool Ray::hasIntersect() {
	return false;
}