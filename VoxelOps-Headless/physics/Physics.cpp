#include "Physics.hpp"

using namespace JPH;
using namespace JPH::literals;



// Layer filters
class Physics::ObjectLayerPairFilterImpl : public ObjectLayerPairFilter
{
public:
    bool ShouldCollide(ObjectLayer inObject1, ObjectLayer inObject2) const override
    {
        switch (inObject1)
        {
        case Layers::NON_MOVING: return inObject2 == Layers::MOVING;
        case Layers::MOVING: return true;
        default: JPH_ASSERT(false); return false;
        }
    }
};

namespace BroadPhaseLayers
{
    static constexpr BroadPhaseLayer NON_MOVING(0);
    static constexpr BroadPhaseLayer MOVING(1);
    static constexpr uint NUM_LAYERS(2);
}

class Physics::BPLayerInterfaceImpl final : public BroadPhaseLayerInterface
{
public:
    BPLayerInterfaceImpl()
    {
        mObjectToBroadPhase[Layers::NON_MOVING] = BroadPhaseLayers::NON_MOVING;
        mObjectToBroadPhase[Layers::MOVING] = BroadPhaseLayers::MOVING;
    }

    uint GetNumBroadPhaseLayers() const override
    {
        return BroadPhaseLayers::NUM_LAYERS;
    }

    BroadPhaseLayer GetBroadPhaseLayer(ObjectLayer inLayer) const override
    {
        JPH_ASSERT(inLayer < Layers::NUM_LAYERS);
        return mObjectToBroadPhase[inLayer];
    }

#if defined(JPH_EXTERNAL_PROFILE) || defined(JPH_PROFILE_ENABLED)
    const char* GetBroadPhaseLayerName(BroadPhaseLayer inLayer) const override
    {
        switch ((BroadPhaseLayer::Type)inLayer)
        {
        case (BroadPhaseLayer::Type)BroadPhaseLayers::NON_MOVING: return "NON_MOVING";
        case (BroadPhaseLayer::Type)BroadPhaseLayers::MOVING:     return "MOVING";
        default:                                                  JPH_ASSERT(false); return "INVALID";
        }
    }
#endif

private:
    BroadPhaseLayer mObjectToBroadPhase[Layers::NUM_LAYERS];
};

class Physics::ObjectVsBroadPhaseLayerFilterImpl : public ObjectVsBroadPhaseLayerFilter
{
public:
    bool ShouldCollide(ObjectLayer inLayer1, BroadPhaseLayer inLayer2) const override
    {
        switch (inLayer1)
        {
        case Layers::NON_MOVING: return inLayer2 == BroadPhaseLayers::MOVING;
        case Layers::MOVING: return true;
        default: JPH_ASSERT(false); return false;
        }
    }
};

class Physics::MyBodyActivationListener : public BodyActivationListener
{
public:
    void OnBodyActivated(const BodyID&, uint64) override { std::cout << "Body activated" << std::endl; }
    void OnBodyDeactivated(const BodyID&, uint64) override { std::cout << "Body deactivated" << std::endl; }
};

class Physics::MyContactListener : public ContactListener
{
public:
    ValidateResult OnContactValidate(const Body&, const Body&, RVec3Arg, const CollideShapeResult&) override
    {
        return ValidateResult::AcceptAllContactsForThisBodyPair;
    }
    void OnContactAdded(const Body&, const Body&, const ContactManifold&, ContactSettings&) override
    {
        std::cout << "Contact added" << std::endl;
    }
    void OnContactPersisted(const Body&, const Body&, const ContactManifold&, ContactSettings&) override
    {
        //std::cout << "Contact persisted" << std::endl;
    }
    void OnContactRemoved(const SubShapeIDPair&) override
    {
        std::cout << "Contact removed" << std::endl;
    }
};

// Tracing and asserts
static void TraceImpl(const char* inFMT, ...)
{
    va_list list;
    va_start(list, inFMT);
    char buffer[1024];
    vsnprintf(buffer, sizeof(buffer), inFMT, list);
    va_end(list);
    std::cout << buffer << std::endl;
}

#ifdef JPH_ENABLE_ASSERTS
static bool AssertFailedImpl(const char* inExpression, const char* inMessage, const char* inFile, uint inLine)
{
    std::cout << inFile << ":" << inLine << ": (" << inExpression << ") " << (inMessage ? inMessage : "") << std::endl;
    return true;
}
#endif

// -----------------
// Physics class code
// -----------------

Physics::Physics()
{
    // Initialize Jolt
    RegisterDefaultAllocator();
    Trace = TraceImpl;
    JPH_IF_ENABLE_ASSERTS(AssertFailed = AssertFailedImpl;)
        Factory::sInstance = new Factory();
    RegisterTypes();

    // Create temp allocator and job system
    mTempAllocator = std::make_unique<TempAllocatorImpl>(10 * 1024 * 1024);
    mJobSystem = std::make_unique<JobSystemThreadPool>(cMaxPhysicsJobs, cMaxPhysicsBarriers, thread::hardware_concurrency() - 1);

    // Create filters
    mBroadPhaseLayerInterface = std::make_unique<BPLayerInterfaceImpl>();
    mObjectVsBroadPhaseLayerFilter = std::make_unique<ObjectVsBroadPhaseLayerFilterImpl>();
    mObjectLayerPairFilter = std::make_unique<ObjectLayerPairFilterImpl>();

    // Init physics system
    mPhysicsSystem.Init(
        1024, // Max bodies
        0,    // Mutex count
        1024, // Max body pairs
        1024, // Max contact constraints
        *mBroadPhaseLayerInterface,
        *mObjectVsBroadPhaseLayerFilter,
        *mObjectLayerPairFilter
    );

    // Setup listeners
    mBodyActivationListener = std::make_unique<MyBodyActivationListener>();
    mContactListener = std::make_unique<MyContactListener>();
    mPhysicsSystem.SetBodyActivationListener(mBodyActivationListener.get());
    mPhysicsSystem.SetContactListener(mContactListener.get());

    // Create the floor
    BodyInterface& bodyInterface = mPhysicsSystem.GetBodyInterface();
    BoxShapeSettings floorShapeSettings(Vec3(1000.0f, 1.0f, 1000.0f));
    floorShapeSettings.SetEmbedded();
    ShapeRefC floorShape = floorShapeSettings.Create().Get();
    BodyCreationSettings floorSettings(floorShape, RVec3(0.0_r, -1.0_r, 0.0_r), Quat::sIdentity(), EMotionType::Static, Layers::NON_MOVING);

    floorBodyID = bodyInterface.CreateAndAddBody(floorSettings, EActivation::DontActivate);

    mPhysicsSystem.OptimizeBroadPhase();
}

Physics::~Physics()
{
    BodyInterface& bodyInterface = mPhysicsSystem.GetBodyInterface();

    // Cleanup
    UnregisterTypes();
    delete Factory::sInstance;
    Factory::sInstance = nullptr;
}

void Physics::update(float deltaTime)
{
    mPhysicsSystem.Update(deltaTime, 1, mTempAllocator.get(), mJobSystem.get());
}
