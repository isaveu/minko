/*
Copyright (c) 2013 Aerys

Permission is hereby granted, free of charge, to any person obtaining a copy of this software and
associated documentation files (the "Software"), to deal in the Software without restriction,
including without limitation the rights to use, copy, modify, merge, publish, distribute,
sublicense, and/or sell copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all copies or
substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING
BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM,
DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
*/

#include <bitset>
#include "minko/component/bullet/PhysicsWorld.hpp"

#include <btBulletDynamicsCommon.h>
#include <minko/math/Matrix4x4.hpp>
#include <minko/scene/Node.hpp>
#include <minko/scene/NodeSet.hpp>
#include <minko/component/SceneManager.hpp>
#include <minko/component/Renderer.hpp>
#include <minko/component/bullet/LinearIdAllocator.hpp>
#include <minko/component/bullet/ColliderData.hpp>
#include <minko/component/bullet/AbstractPhysicsShape.hpp>

#include "minko/math/tools.hpp"

using namespace minko;
using namespace minko::math;
using namespace minko::scene;
using namespace minko::component;

/*static*/ const uint bullet::PhysicsWorld::_MAX_BODIES = 2048;


bullet::PhysicsWorld::PhysicsWorld():
	AbstractComponent(),
	_uidAllocator(LinearIdAllocator::create(_MAX_BODIES)),
	_colliderMap(),
	_colliderReverseMap(),
	_uidToCollider(),
	_collisions(),
	_bulletBroadphase(nullptr),
	_bulletCollisionConfiguration(nullptr),
	_bulletConstraintSolver(nullptr),
	_bulletDispatcher(nullptr),
	_bulletDynamicsWorld(nullptr),
	_targetAddedSlot(nullptr),
	_targetRemovedSlot(nullptr),
	_frameBeginSlot(nullptr),
	_frameEndSlot(nullptr),
	_componentAddedOrRemovedSlot(nullptr),
	_addedOrRemovedSlot(nullptr),
    _colliderGroupChangedSlot(),
    _colliderMaskChangedSlot()
{
}


void 
bullet::PhysicsWorld::initialize()
{
	// straightforward physics world initialization for the time being
	_bulletBroadphase				= std::shared_ptr<btDbvtBroadphase>(new btDbvtBroadphase());
	_bulletCollisionConfiguration	= std::shared_ptr<btDefaultCollisionConfiguration>(new btDefaultCollisionConfiguration());
	_bulletConstraintSolver			= std::shared_ptr<btSequentialImpulseConstraintSolver>(new btSequentialImpulseConstraintSolver());
	_bulletDispatcher				= std::shared_ptr<btCollisionDispatcher>(new btCollisionDispatcher(_bulletCollisionConfiguration.get()));

	_bulletDynamicsWorld			= std::shared_ptr<btDiscreteDynamicsWorld>(new btDiscreteDynamicsWorld(
		_bulletDispatcher.get(), 
		_bulletBroadphase.get(),
		_bulletConstraintSolver.get(),
		_bulletCollisionConfiguration.get()
		));

	_targetAddedSlot	= targetAdded()->connect(std::bind(
		&bullet::PhysicsWorld::targetAddedHandler,
		shared_from_this(),
		std::placeholders::_1,
		std::placeholders::_2
	));

	_targetRemovedSlot	= targetRemoved()->connect(std::bind(
		&bullet::PhysicsWorld::targetRemovedHandler,
		shared_from_this(),
		std::placeholders::_1,
		std::placeholders::_2
	));
}

void 
bullet::PhysicsWorld::targetAddedHandler(AbstractComponent::Ptr controller, 
										 Node::Ptr				target)
{
	if (targets().size() > 1)
		throw std::logic_error("The same PhysicsWorld cannot be used twice.");

	setSceneManager(target->root()->component<SceneManager>());
}

void 
bullet::PhysicsWorld::targetRemovedHandler(AbstractComponent::Ptr	controller, 
										   Node::Ptr				target)
{
	_sceneManager					= nullptr;
	_frameBeginSlot					= nullptr;
	_frameEndSlot					= nullptr;
	_addedOrRemovedSlot				= nullptr;
	_componentAddedOrRemovedSlot	= nullptr;
	_exitFrameSlot					= nullptr;

	_colliderMap.clear();
	_colliderReverseMap.clear();
	_uidToCollider.clear();
}

void
bullet::PhysicsWorld::setSceneManager(std::shared_ptr<SceneManager> sceneManager)
{
	if (sceneManager != _sceneManager || (!_componentAddedOrRemovedSlot && !_addedOrRemovedSlot))
	{
		auto target = targets()[0];
		auto componentCallback = [&](NodePtr node, NodePtr target, AbsCtrlPtr cmp)
		{
			setSceneManager(target->root()->component<SceneManager>());
		};
		auto nodeCallback = [&](NodePtr node, NodePtr target, NodePtr ancestor)
		{
			setSceneManager(target->root()->component<SceneManager>());
		};

		if (sceneManager)
		{
			_sceneManager = sceneManager;
			_frameBeginSlot = sceneManager->frameBegin()->connect(std::bind(
				&PhysicsWorld::frameBeginHandler, shared_from_this(), std::placeholders::_1, std::placeholders::_2, std::placeholders::_3
			));
			_frameEndSlot = sceneManager->frameEnd()->connect(std::bind(
				&PhysicsWorld::frameEndHandler, shared_from_this(), std::placeholders::_1, std::placeholders::_2, std::placeholders::_3
			));

			_componentAddedOrRemovedSlot = target->componentRemoved()->connect(componentCallback);
			_addedOrRemovedSlot = target->removed()->connect(nodeCallback);
		}
		else
		{
			_sceneManager	= nullptr;
			_frameBeginSlot	= nullptr;
			_frameEndSlot	= nullptr;

			_componentAddedOrRemovedSlot = target->componentAdded()->connect(componentCallback);
			_addedOrRemovedSlot = target->added()->connect(nodeCallback);
		}
	}
}

void
bullet::PhysicsWorld::addChild(ColliderData::Ptr data)
{
    if (data == nullptr || data->node() == nullptr)
        throw std::invalid_argument("data");

	if (hasCollider(data))
		throw std::logic_error("The same data cannot be added twice.");

	//std::cout << "PhysicsWorld::addChild\tnode '" << data->node()->name() 
	//	<< "'\tgroup = " << std::bitset<16>(data->collisionGroup()) 
	//	<< "\tmask = " << std::bitset<16>(data->collisionMask()) 
	//	<< std::endl;

	data->uid(_uidAllocator->allocate());

	_uidToCollider.insert(std::pair<uint, ColliderData::Ptr>(data->uid(), data));

	BulletCollider::Ptr bulletCollider = BulletCollider::create(data);
	_colliderMap.insert(std::pair<ColliderData::Ptr, BulletCollider::Ptr>(data, bulletCollider));
	_colliderReverseMap.insert(std::pair<btCollisionObject*, ColliderData::Ptr>(bulletCollider->rigidBody().get(), data));


	_colliderGroupChangedSlot[data]	= data->node()->layoutsChanged()->connect([&](Node::Ptr, Node::Ptr){ updateCollisionFilter(data); });
    _colliderMaskChangedSlot[data]	= data->collisionFilterChanged()->connect([&](ColliderData::Ptr){ updateCollisionFilter(data); });


	std::dynamic_pointer_cast<btDiscreteDynamicsWorld>(_bulletDynamicsWorld)
		->addRigidBody(
            bulletCollider->rigidBody().get(),
            //short(data->node()->layouts() & ((1<<16) - 1)), // FIXME
            data->collisionGroup(),
            data->collisionMask()
         );

#ifdef DEBUG_PHYSICS
	std::cout << "[" << data->name() << "]\tadd physics body" << std::endl;

	print(std::cout << "rigidbody.worldTransform =\n", bulletCollider->rigidBody()->getWorldTransform()) << std::endl;
#endif // DEBUG_PHYSICS
}

void
bullet::PhysicsWorld::updateCollisionFilter(ColliderData::Ptr data)
{
    auto foundColliderIt = _colliderMap.find(data);
    if (foundColliderIt != _colliderMap.end())
    {
        auto proxy = foundColliderIt->second->rigidBody()->getBroadphaseProxy();
        
        //proxy->m_collisionFilterGroup   = short(data->node()->layouts() & ((1<<16) - 1)); // FIXME
        proxy->m_collisionFilterGroup   = data->collisionGroup();
        proxy->m_collisionFilterMask    = data->collisionMask();
    }
}

void
bullet::PhysicsWorld::removeChild(ColliderData::Ptr data)
{
    if (_colliderGroupChangedSlot.count(data))
        _colliderGroupChangedSlot.erase(data);

    if (_colliderMaskChangedSlot.count(data))
        _colliderMaskChangedSlot.erase(data);

	auto bulletColliderIt = _colliderMap.find(data);
	if (bulletColliderIt != _colliderMap.end())
	{
		btCollisionObject*	bulletObject = bulletColliderIt->second->rigidBody().get();

		auto dataIt = _colliderReverseMap.find(bulletObject);

		if (dataIt != _colliderReverseMap.end())
			_colliderReverseMap.erase(dataIt);
			
		_bulletDynamicsWorld->removeCollisionObject(bulletObject);
		_colliderMap.erase(bulletColliderIt);
	}

	auto uidIt = _uidToCollider.find(data->uid());
	if (uidIt != _uidToCollider.end())
	{
		_uidAllocator->free(data->uid());
		_uidToCollider.erase(uidIt);
	}

	// remove all current collision pairs the collider appears in (warning: it is an ordered set, remove_if won't work).
	for (auto collisionIt = _collisions.begin(); collisionIt != _collisions.end(); )
		if (collisionIt->first == data->uid() ||
			collisionIt->second == data->uid())
			collisionIt = _collisions.erase(collisionIt);
		else
			++collisionIt;
}

bool
bullet::PhysicsWorld::hasCollider(ColliderData::Ptr data) const
{
	return _colliderMap.find(data) != _colliderMap.end();
}


void
bullet::PhysicsWorld::setGravity(Vector3::Ptr gravity)
{
	_bulletDynamicsWorld->setGravity(btVector3(gravity->x(), gravity->y(), gravity->z()));
}

void
bullet::PhysicsWorld::frameBeginHandler(std::shared_ptr<SceneManager> sceneManager, float time, float deltaTime)
{
	_bulletDynamicsWorld->stepSimulation(deltaTime);
	updateColliders();
}

void
bullet::PhysicsWorld::frameEndHandler(std::shared_ptr<SceneManager> sceneManager, float time, float deltaTime)
{
	notifyCollisions();
}

void
bullet::PhysicsWorld::updateColliders()
{
	static auto physicsModelToWorld = Matrix4x4::create();

	for (auto& dataAndBulletCollider : _colliderMap)
	{
		auto data				= dataAndBulletCollider.first;
		auto bulletCollider		= dataAndBulletCollider.second;

		if (data->isStatic())
			continue;

		fromBulletTransform(
			bulletCollider->rigidBody()->getWorldTransform(),
			physicsModelToWorld
		);

		data->updatePhysicsTransform(physicsModelToWorld); // triggers updating signals
	}
}

void
bullet::PhysicsWorld::updateRigidBodyState(ColliderData::Ptr	data, 
										   Matrix4x4::Ptr		graphicsNoScaleTransform, 
										   Matrix4x4::Ptr		centerOfMassOffset)
{
#ifdef DEBUG
	const float det3x3 = fabsf(graphicsNoScaleTransform->determinant3x3());
	if (fabsf(det3x3 - 1.0f) > 1e-3f)
		throw std::logic_error("Graphics world matrices used for updating rigid bodies' must be pure rotation + translation matrices.");
#endif // DEBUG

	auto foundDataIt = _colliderMap.find(data);
	if (foundDataIt == _colliderMap.end())
		return;

	auto					bulletCollider		= foundDataIt->second;
	btDefaultMotionState*	bulletMotionState	= dynamic_cast<btDefaultMotionState*>(bulletCollider->rigidBody()->getMotionState());

	if (bulletMotionState == nullptr)
		return;

	// update the motion state's center of mass offset transform
	toBulletTransform(centerOfMassOffset,		bulletMotionState->m_centerOfMassOffset);

	// update the motion state's world transform
	toBulletTransform(graphicsNoScaleTransform,	bulletMotionState->m_graphicsWorldTrans);

	// synchronize bullet
	static btTransform bulletTransform;

	bulletMotionState->getWorldTransform(bulletTransform);
	bulletCollider->rigidBody()->setWorldTransform(bulletTransform);
}

void
bullet::PhysicsWorld::notifyCollisions()
{
	CollisionSet		currentCollisions;
	ColliderData::Ptr	colliderData[2]	= { nullptr, nullptr };
	const int			numManifolds	= _bulletDynamicsWorld->getDispatcher()->getNumManifolds();

	for (int i = 0; i < numManifolds; ++i)
	{
		btPersistentManifold* manifold	= _bulletDynamicsWorld->getDispatcher()->getManifoldByIndexInternal(i);

		auto colliderDataIt = _colliderReverseMap.find(manifold->getBody0());
		colliderData[0]		= colliderDataIt != _colliderReverseMap.end()
			? colliderDataIt->second
			: nullptr;

		colliderDataIt		= _colliderReverseMap.find(manifold->getBody1());
		colliderData[1]		= colliderDataIt != _colliderReverseMap.end()
			? colliderDataIt->second
			: nullptr;

		if (colliderData[0] == nullptr || colliderData[1] == nullptr)
			continue;
		//if (!colliderData[0]->triggerCollisions() && !colliderData[1]->triggerCollisions())
		//	continue;
		
		// a collision exists between to valid colliders
		auto collision = std::make_pair(colliderData[0]->uid(),  colliderData[1]->uid());

		if (collision.first > collision.second)
			std::swap(collision.first, collision.second);

		if (_collisions.find(collision) == _collisions.end()) // inserted only once
		{
			if (colliderData[0]->triggerCollisions())
				colliderData[0]->collisionStarted()->execute(colliderData[0], colliderData[1]);
			if (colliderData[1]->triggerCollisions())
				colliderData[1]->collisionStarted()->execute(colliderData[1], colliderData[0]);
		}

		currentCollisions.insert(collision);
	}

	// FIXME: not optimal at all.
	// find and notify collisions that are not present anymore as the difference with the intersection
	// between the previous collision set and the current one.

	CollisionSet lostCollisions;
	std::set_difference(
		_collisions.begin(), _collisions.end(), 
		currentCollisions.begin(), currentCollisions.end(), 
		std::inserter(lostCollisions, lostCollisions.end())
	);

	for (auto& collision : lostCollisions)
	{
		auto colliderDataIt = _uidToCollider.find(collision.first);
		colliderData[0]		= colliderDataIt != _uidToCollider.end()
			? colliderDataIt->second
			: nullptr;

		colliderDataIt		= _uidToCollider.find(collision.second);
		colliderData[1]		= colliderDataIt != _uidToCollider.end()
			? colliderDataIt->second
			: nullptr;

		if (colliderData[0] == nullptr || colliderData[1] == nullptr)
			continue;

		// colliders assured to trigger collisions at this point.
		colliderData[0]->collisionEnded()->execute(colliderData[0], colliderData[1]);
		colliderData[1]->collisionEnded()->execute(colliderData[1], colliderData[0]);
	}
	
	_collisions.swap(currentCollisions);
}

