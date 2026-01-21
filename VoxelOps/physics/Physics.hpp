#pragma once

#include <Jolt/Jolt.h>

// Jolt includes
#include <Jolt/RegisterTypes.h>
#include <Jolt/Core/Factory.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Core/JobSystemThreadPool.h>
#include <Jolt/Physics/PhysicsSettings.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Body/BodyActivationListener.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/ShapeCast.h>
#include <Jolt/Physics/Collision/RayCast.h> 
#include <Jolt/Physics/Collision/CastResult.h>
#include <Jolt/Physics/Collision/CollisionCollectorImpl.h>
#include <iostream>
#include <cstdarg>
#include <thread>

namespace Layers
{
    static constexpr JPH::ObjectLayer NON_MOVING = 0;
    static constexpr JPH::ObjectLayer MOVING = 1;
    static constexpr JPH::ObjectLayer NUM_LAYERS = 2;
}

class Physics
{
public:
    Physics();
    ~Physics();

    void update(float deltaTime);

    JPH::PhysicsSystem& getPhysicsSystem() { return mPhysicsSystem; }

    JPH::BodyID floorBodyID;
private:
    std::unique_ptr<JPH::TempAllocatorImpl> mTempAllocator;
    std::unique_ptr<JPH::JobSystemThreadPool> mJobSystem;
    JPH::PhysicsSystem mPhysicsSystem;

    class MyBodyActivationListener;
    class MyContactListener;
    class ObjectLayerPairFilterImpl;
    class BPLayerInterfaceImpl;
    class ObjectVsBroadPhaseLayerFilterImpl;

    std::unique_ptr<MyBodyActivationListener> mBodyActivationListener;
    std::unique_ptr<MyContactListener> mContactListener;
    std::unique_ptr<ObjectLayerPairFilterImpl> mObjectLayerPairFilter;
    std::unique_ptr<BPLayerInterfaceImpl> mBroadPhaseLayerInterface;
    std::unique_ptr<ObjectVsBroadPhaseLayerFilterImpl> mObjectVsBroadPhaseLayerFilter;

};
