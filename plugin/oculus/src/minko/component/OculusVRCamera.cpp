/*
Copyright (c) 2014 Aerys

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

#include "minko/component/OculusVRCamera.hpp"
#include "minko/MinkoOculus.hpp"

#include "OVR_CAPI.h"
#include "Kernel/OVR_Math.h"

#include "minko/scene/Node.hpp"
#include "minko/scene/NodeSet.hpp"
#include "minko/component/AbstractComponent.hpp"
#include "minko/component/SceneManager.hpp"
#include "minko/component/Renderer.hpp"
#include "minko/component/PerspectiveCamera.hpp"
#include "minko/component/Transform.hpp"
#include "minko/component/Surface.hpp"
#include "minko/geometry/QuadGeometry.hpp"
#include "minko/data/StructureProvider.hpp"
#include "minko/render/Texture.hpp"
#include "minko/file/AssetLibrary.hpp"
#include "minko/math/Matrix4x4.hpp"
#include "minko/render/Effect.hpp"
#include "minko/material/Material.hpp"

using namespace minko;
using namespace minko::scene;
using namespace minko::component;
using namespace minko::math;

/*static*/ const float                    OculusVRCamera::WORLD_UNIT    = 1.0f;
/*static*/ const unsigned int            OculusVRCamera::TARGET_SIZE    = 1024;

OculusVRCamera::OculusVRCamera(float aspectRatio, float zNear, float zFar) :
    _aspectRatio(aspectRatio),
    _zNear(zNear),
    _zFar(zFar),
    _eyePosition(Vector3::create(0.0f, 0.0f, 0.0f)),
    _eyeOrientation(Matrix4x4::create()),
    _sceneManager(nullptr),
    _leftCamera(nullptr),
    _leftRenderer(nullptr),
    _rightCamera(nullptr),
    _rightRenderer(nullptr),
    _targetAddedSlot(nullptr),
    _targetRemovedSlot(nullptr),
    _addedSlot(nullptr),
    _removedSlot(nullptr),
    _renderEndSlot(nullptr)
{

}

OculusVRCamera::~OculusVRCamera()
{
    resetOVRDevice();

    ovrHmd_Destroy(_hmd);
    ovr_Shutdown();
}

void
OculusVRCamera::initialize()
{
    _targetAddedSlot = targetAdded()->connect(std::bind(
        &OculusVRCamera::targetAddedHandler,
        std::static_pointer_cast<OculusVRCamera>(shared_from_this()),
        std::placeholders::_1,
        std::placeholders::_2
    ));

    _targetRemovedSlot = targetRemoved()->connect(std::bind(
        &OculusVRCamera::targetRemovedHandler,
        std::static_pointer_cast<OculusVRCamera>(shared_from_this()),
        std::placeholders::_1,
        std::placeholders::_2
    ));

    initializeOVRDevice();
}

void
OculusVRCamera::targetAddedHandler(AbsCmpPtr component, NodePtr target)
{
    if (targets().size() > 1)
        throw std::logic_error("The OculusVRCamera component cannot have more than 1 target.");

    _addedSlot = target->added()->connect(std::bind(
        &OculusVRCamera::findSceneManager,
        std::static_pointer_cast<OculusVRCamera>(shared_from_this())
    ));

    _removedSlot = target->removed()->connect(std::bind(
        &OculusVRCamera::findSceneManager,
        std::static_pointer_cast<OculusVRCamera>(shared_from_this())
    ));

    initializeCameras();

    findSceneManager();
}

void
OculusVRCamera::targetRemovedHandler(AbsCmpPtr component, NodePtr target)
{
    findSceneManager();
}

void
OculusVRCamera::resetOVRDevice()
{
    //_ovrHMDDevice = nullptr;
    //_ovrSensorDevice = nullptr;
    //_ovrSensorFusion = nullptr;
}

void
OculusVRCamera::initializeOVRDevice()
{
    ovr_Initialize();

    resetOVRDevice();

    _hmd = ovrHmd_Create(0);
    if (!_hmd)
        throw;

    OVR::Sizei recommenedTex0Size = ovrHmd_GetFovTextureSize(_hmd, ovrEye_Left, _hmd->DefaultEyeFov[0], 1.0f);
    OVR::Sizei recommenedTex1Size = ovrHmd_GetFovTextureSize(_hmd, ovrEye_Right, _hmd->DefaultEyeFov[1], 1.0f);
    OVR::Sizei renderTargetSize;
    renderTargetSize.w = recommenedTex0Size.w + recommenedTex1Size.w;
    renderTargetSize.h = std::max(recommenedTex0Size.h, recommenedTex1Size.h);

    // enforce a power of 2 size because that's what minko expects
    renderTargetSize.w = math::clp2(renderTargetSize.w);
    renderTargetSize.h = math::clp2(renderTargetSize.h);

    renderTargetSize.w = std::min(renderTargetSize.w, MINKO_PLUGIN_OCULUS_MAX_TARGET_SIZE);
    renderTargetSize.h = std::min(renderTargetSize.h, MINKO_PLUGIN_OCULUS_MAX_TARGET_SIZE);

    _renderTargetWidth = renderTargetSize.w;
    _renderTargetHeight = renderTargetSize.h;

    // compute each viewport pos and size
    ovrRecti eyeRenderViewport[2];
    ovrFovPort eyeFov[2] = { _hmd->DefaultEyeFov[0], _hmd->DefaultEyeFov[1] };

    eyeRenderViewport[0].Pos = OVR::Vector2i(0, 0);
    eyeRenderViewport[0].Size = OVR::Sizei(renderTargetSize.w / 2, renderTargetSize.h);
    eyeRenderViewport[1].Pos = OVR::Vector2i((renderTargetSize.w + 1) / 2, 0);
    eyeRenderViewport[1].Size = eyeRenderViewport[0].Size;

    // create 1 renderer/eye and init. their respective viewport
    _leftRenderer = Renderer::create();
    _leftRenderer->viewport(
        eyeRenderViewport[0].Pos.x,
        eyeRenderViewport[0].Pos.y,
        eyeRenderViewport[0].Size.w,
        eyeRenderViewport[0].Size.h
    );

    _rightRenderer = Renderer::create();
    _rightRenderer->viewport(
        eyeRenderViewport[1].Pos.x,
        eyeRenderViewport[1].Pos.y,
        eyeRenderViewport[1].Size.w,
        eyeRenderViewport[1].Size.h
    );
    _rightRenderer->clearBeforeRender(false);

    ovrHmd_SetEnabledCaps(_hmd, ovrHmdCap_LowPersistence | ovrHmdCap_DynamicPrediction);

    // Start the sensor which informs of the Rift's pose and motion
    ovrHmd_ConfigureTracking(
        _hmd,
        ovrTrackingCap_Orientation |
        ovrTrackingCap_MagYawCorrection |
        ovrTrackingCap_Position,
        0
    );

    // FIXME: on Windows, render directly into the HMD (window ?= SDL window)
    // ovrHmd_AttachToWindow(HMD, window, NULL, NULL);
}

void
OculusVRCamera::initializeCameras()
{
    auto aspectRatio = (float)_renderTargetWidth / (float)_renderTargetHeight;

    _leftCamera = PerspectiveCamera::create(
        aspectRatio,
        atan(_hmd->DefaultEyeFov[0].LeftTan + _hmd->DefaultEyeFov[0].RightTan),
        _zNear,
        _zFar
    );
    auto leftCameraNode = scene::Node::create("oculusLeftEye")
        ->addComponent(Transform::create())
        ->addComponent(_leftCamera)
        ->addComponent(_leftRenderer);
    targets()[0]->addChild(leftCameraNode);

    _rightCamera = PerspectiveCamera::create(
        aspectRatio,
        atan(_hmd->DefaultEyeFov[1].LeftTan + _hmd->DefaultEyeFov[1].RightTan),
        _zNear,
        _zFar
    );
    auto rightCameraNode = scene::Node::create("oculusRightEye")
        ->addComponent(Transform::create())
        ->addComponent(_rightCamera)
        ->addComponent(_rightRenderer);
    targets()[0]->addChild(rightCameraNode);
}

void
OculusVRCamera::initializeDistortionGeometry(std::shared_ptr<render::AbstractContext> context)
{
    for (int eyeNum = 0; eyeNum < 2; eyeNum++)
    {
        auto geom = geometry::Geometry::create();
        
        // Allocate mesh vertices, registering with renderer using the OVR vertex format.
        ovrDistortionMesh meshData;
        ovrHmd_CreateDistortionMesh(
            _hmd,
            (ovrEyeType)eyeNum,
            _hmd->DefaultEyeFov[eyeNum],
            ovrDistortionCap_Chromatic | ovrDistortionCap_TimeWarp,
            &meshData
        );

        auto vb = render::VertexBuffer::create(
            context,
            (float*)meshData.pVertexData,
            (sizeof(ovrDistortionVertex) / sizeof(float))
            * meshData.VertexCount
        );

        // struct ovrDistortionVertex {
        //     ovrVector2f ScreenPosNDC;    // [-1,+1],[-1,+1] over the entire framebuffer.
        //     float       TimeWarpFactor;  // Lerp factor between time-warp matrices. Can be encoded in Pos.z.
        //     float       VignetteFactor;  // Vignette fade factor. Can be encoded in Pos.w.
        //     ovrVector2f TanEyeAnglesR;
        //     ovrVector2f TanEyeAnglesG;
        //     ovrVector2f TanEyeAnglesB;
        // }

        vb->addAttribute("screenPosNDC", 2);
        vb->addAttribute("timeWarpFactor", 1);
        vb->addAttribute("vignetteFactor", 1);
        vb->addAttribute("tanEyeAnglesR", 2);
        vb->addAttribute("tanEyeAnglesG", 2);
        vb->addAttribute("tanEyeAnglesB", 2);
        
        geom->addVertexBuffer(vb);

        auto ib = render::IndexBuffer::create(
            context,
            meshData.pIndexData,
            meshData.pIndexData + meshData.IndexCount * sizeof(unsigned short)
        );

        geom->indices(ib);
    }
}

void
OculusVRCamera::getHMDInfo(HMDInfo& _hmdInfo) const
{
    /*if (_ovrHMDDevice)
    {
        OVR::HMDInfo ovrHMDInfo;

        _ovrHMDDevice->GetDeviceInfo(&ovrHMDInfo);

        _hmdInfo.hResolution                = (float)ovrHMDInfo.HResolution;
        _hmdInfo.vResolution                = (float)ovrHMDInfo.VResolution;
        _hmdInfo.hScreenSize                = ovrHMDInfo.HScreenSize;
        _hmdInfo.vScreenSize                = ovrHMDInfo.VScreenSize;
        _hmdInfo.vScreenCenter            = ovrHMDInfo.VScreenCenter;
        _hmdInfo.interpupillaryDistance    = ovrHMDInfo.InterpupillaryDistance;
        _hmdInfo.lensSeparationDistance    = ovrHMDInfo.LensSeparationDistance;
        _hmdInfo.eyeToScreenDistance        = ovrHMDInfo.EyeToScreenDistance;
        _hmdInfo.distortionK                = Vector4::create(ovrHMDInfo.DistortionK[0], ovrHMDInfo.DistortionK[1], ovrHMDInfo.DistortionK[2], ovrHMDInfo.DistortionK[3]);
    }
    else
    {
        _hmdInfo.hResolution                = 1280.0f;
        _hmdInfo.vResolution                = 800.0f;
        _hmdInfo.hScreenSize                = 0.14976f;
        _hmdInfo.vScreenSize                = _hmdInfo.hScreenSize / (1280.0f / 800.0f);
        _hmdInfo.vScreenCenter            = 0.5f * _hmdInfo.vScreenSize;
        _hmdInfo.interpupillaryDistance    = 0.064f;
        _hmdInfo.lensSeparationDistance    = 0.0635f;
        _hmdInfo.eyeToScreenDistance        = 0.041f;
        _hmdInfo.distortionK                = Vector4::create(1.0f, 0.22f, 0.24f, 0.0f);
    }*/
}

void
OculusVRCamera::findSceneManager()
{
    NodeSet::Ptr roots = NodeSet::create(targets())
        ->roots()
        ->where([](NodePtr node)
        {
            return node->hasComponent<SceneManager>();
        });

    if (roots->nodes().size() > 1)
        throw std::logic_error("OculusVRCamera cannot be in two separate scenes.");
    else if (roots->nodes().size() == 1)
        setSceneManager(roots->nodes()[0]->component<SceneManager>());
    else
        setSceneManager(nullptr);
}

void
OculusVRCamera::setSceneManager(SceneManager::Ptr sceneManager)
{
    if (_sceneManager == sceneManager)
        return;

    auto context = sceneManager->assets()->context();

    _renderTarget = render::Texture::create(context, _renderTargetWidth, _renderTargetHeight);
    /*_leftRenderer->target(_renderTarget);
    _rightRenderer->target(_renderTarget);*/

    initializeDistortionGeometry(context);

    // FIXME
}

void
OculusVRCamera::renderEndHandler(std::shared_ptr<SceneManager>    sceneManager,
                                 uint                            frameId,
                                 render::AbstractTexture::Ptr    renderTarget)
{
    updateCameraOrientation();

    /*_leftRenderer->render(sceneManager->assets()->context());
    _rightRenderer->render(sceneManager->assets()->context());*/
}

/*static*/
float
OculusVRCamera::distort(float r, Vector4::Ptr distortionK)
{
    const float r2 = r * r;
    const float r4 = r2 * r2;
    const float r6 = r4 * r2;

    return r * (
        distortionK->x()
        + r2 * distortionK->y()
        + r4 * distortionK->z()
        + r6 * distortionK->w()
    );
}

bool
OculusVRCamera::HMDDeviceDetected() const
{
    //return _ovrHMDDevice != nullptr;
    return false;
}

bool
OculusVRCamera::sensorDeviceDetected() const
{
    //return _ovrSensorDevice != nullptr;
    return false;
}

void
OculusVRCamera::updateCameraOrientation()
{
    /*if (_ovrSensorFusion == nullptr || _targetTransform == nullptr)
        return;

    const OVR::Quatf&    measurement    = _ovrSensorFusion->GetPredictedOrientation();
    auto                quaternion    = math::Quaternion::create(measurement.x, measurement.y, measurement.z, measurement.w);

    quaternion->toMatrix(_eyeOrientation);

    _targetTransform->matrix()->copyTranslation(_eyePosition);

    _targetTransform->matrix()
        ->lock()
        ->copyFrom(_eyeOrientation)
        ->appendTranslation(_eyePosition)
        ->unlock();*/
}

void
OculusVRCamera::resetHeadTracking()
{
    /*if (_ovrSensorFusion)
        _ovrSensorFusion->Reset();*/
}