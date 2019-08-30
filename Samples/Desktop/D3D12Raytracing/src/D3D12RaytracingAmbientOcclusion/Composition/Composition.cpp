//*********************************************************
//
// Copyright (c) Microsoft. All rights reserved.
// This code is licensed under the MIT License (MIT).
// THIS CODE IS PROVIDED *AS IS* WITHOUT WARRANTY OF
// ANY KIND, EITHER EXPRESS OR IMPLIED, INCLUDING ANY
// IMPLIED WARRANTIES OF FITNESS FOR A PARTICULAR
// PURPOSE, MERCHANTABILITY, OR NON-INFRINGEMENT.
//
//*********************************************************

#include "stdafx.h"
#include "Composition.h"
#include "GameInput.h"
#include "EngineTuning.h"
#include "EngineProfiling.h"
#include "GpuTimeManager.h"
#include "Composition.h"
#include "CompiledShaders\RNGVisualizerCS.hlsl.h"
#include "CompiledShaders\CompositionCS.hlsl.h"
#include "D3D12RaytracingAmbientOcclusion.h"

// ToDo prune unused
using namespace std;
using namespace DX;
using namespace DirectX;
using namespace SceneEnums;

namespace Composition_Args
{
    // ToDo don't render redundant passes?
    // ToDo Modularize parameters?
    // ToDO standardize capitalization

    const WCHAR* CompositionModes[CompositionType::Count] = {
        L"Specular Pathtracer",
        L"Denoised AO",
        L"Temporally Supersampled Ambient Occlusion",
        L"Raw one-frame Ambient Occlusion",
        L"AO and Disocclusion Map",
        L"AO Variance",
        L"AO Local Variance",
        L"Temporal AO Ray Hit Distance", // ToDo  raw as well?
        L"Normal Map",
        L"Depth Buffer",
        L"Diffuse",
        L"Disocclusion Map" 
    };
    EnumVar CompositionMode(L"Render/Render composition/Mode", CompositionType::AmbientOcclusionOnly_TemporallySupersampled, CompositionType::Count, CompositionModes);
    BoolVar Compose_VarianceVisualizeStdDeviation(L"Render/Render composition/Variance/Visualize std deviation", true);
    NumVar Compose_VarianceScale(L"Render/Render composition/Variance/Variance scale", 1.0f, 0, 10, 0.1f);


    const WCHAR* AntialiasingModes[DownsampleFilter::Count] = { L"OFF", L"SSAA 4x (BoxFilter2x2)", L"SSAA 4x (GaussianFilter9Tap)", L"SSAA 4x (GaussianFilter25Tap)" };
#if REPRO_BLOCKY_ARTIFACTS_NONUNIFORM_CB_REFERENCE_SSAO // Disable SSAA as the blockiness gets smaller with higher resoltuion 
    EnumVar AntialiasingMode(L"Render/Antialiasing", DownsampleFilter::None, DownsampleFilter::Count, AntialiasingModes, Sample::OnRecreateRaytracingResources, nullptr);
#else
    EnumVar AntialiasingMode(L"Render/Antialiasing", DownsampleFilter::None, DownsampleFilter::Count, AntialiasingModes, Sample::OnRecreateRaytracingResources, nullptr);
#endif


    BoolVar UpsamplingUseBilinearWeights(L"Render/AO/RTAO/Down/Upsampling/Bilinear weighted", true);
    BoolVar UpsamplingUseDepthWeights(L"Render/AO/RTAO/Down/Upsampling/Depth weighted", true);
    BoolVar UpsamplingUseNormalWeights(L"Render/AO/RTAO/Down/Upsampling/Normal weighted", true);
    BoolVar UpsamplingUseDynamicDepthThreshold(L"Render/AO/RTAO/Down/Upsampling/Dynamic depth threshold", true);        // ToDO rename to adaptive

    // ToDo move under PBR options?
    BoolVar AOEnabled(L"Render/AO/Enabled", true);
}


void Composition::Setup(shared_ptr<DeviceResources> deviceResources, shared_ptr<DX::DescriptorHeap> descriptorHeap)
{
    m_deviceResources = deviceResources;
    m_cbvSrvUavHeap = descriptorHeap;

    CreateDeviceDependentResources();
}

void Composition::Release()
{
    m_csHemisphereVisualizationCB.Release();
}

// Create resources that depend on the device.
void Composition::CreateDeviceDependentResources()
{
    CreateAuxilaryDeviceResources();

    // ToDo move/rename
    CreateComposeRenderPassesCSResources();
}


// ToDo rename
void Composition::CreateAuxilaryDeviceResources()
{
    auto device = m_deviceResources->GetD3DDevice();
    auto FrameCount = m_deviceResources->GetBackBufferCount();

    m_upsampleBilateralFilterKernel.Initialize(device, FrameCount);
}

void Composition::CreateResolutionDependentResources()
{
    CreateTextureResources();
}


void Composition::SetResolution(UINT width, UINT height)
{
    m_renderingWidth = width;
    m_renderingHeight = height;

    CreateResolutionDependentResources();
}

void Composition::CreateTextureResources()
{
    auto device = m_deviceResources->GetD3DDevice();
    D3D12_RESOURCE_STATES initialResourceState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;

    CreateRenderTargetResource(device, RTAO::ResourceFormat(RTAO::ResourceType::AOCoefficient), m_renderingWidth, m_renderingHeight, m_cbvSrvUavHeap.get(), &m_upsampledAOValueResource, initialResourceState, L"Upsampled AO value");
    CreateRenderTargetResource(device, DXGI_FORMAT_R8_UINT, m_renderingWidth, m_renderingHeight, m_cbvSrvUavHeap.get(), &m_upsampledTrppResource, initialResourceState, L"Upsampled Trpp");
    CreateRenderTargetResource(device, RTAO::ResourceFormat(RTAO::ResourceType::RayHitDistance), m_renderingWidth, m_renderingHeight, m_cbvSrvUavHeap.get(), &m_upsampledAORayHitDistanceResource, initialResourceState, L"Upsampled AO Ray Hit Distance");
    CreateRenderTargetResource(device, Denoiser::ResourceFormat(Denoiser::ResourceType::Variance), m_renderingWidth, m_renderingHeight, m_cbvSrvUavHeap.get(), &m_upsampledVarianceResource, initialResourceState, L"Upsampled Variance");
    CreateRenderTargetResource(device, Denoiser::ResourceFormat(Denoiser::ResourceType::LocalMeanVariance), m_renderingWidth, m_renderingHeight, m_cbvSrvUavHeap.get(), &m_upsampledLocalMeanVarianceResource, initialResourceState, L"Upsampled Local Mean Variance");
}

void Composition::CreateComposeRenderPassesCSResources()
{
    auto device = m_deviceResources->GetD3DDevice();
    auto FrameCount = m_deviceResources->GetBackBufferCount();

    // Create root signature.
    {
        using namespace CSRootSignature::ComposeRenderPassesCS;

        CD3DX12_DESCRIPTOR_RANGE ranges[Slot::Count]; // Perfomance TIP: Order from most frequent to least frequent.
        ranges[Slot::Output].Init(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0);  // 1 output texture
        ranges[Slot::GBufferResources].Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 5, 0);  // 5 input GBuffer textures
        ranges[Slot::AO].Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 5);  // 1 input AO texture
        ranges[Slot::AORayHitDistance].Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 9);  // 1 input AO ray hit distance texture
        ranges[Slot::FrameAge].Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 10); // 1 input disocclusion map texture
        ranges[Slot::Color].Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 11); // 1 input color texture
        ranges[Slot::AOSurfaceAlbedo].Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 12); // 1 input AO diffuse texture
        ranges[Slot::Variance].Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 13);
        ranges[Slot::LocalMeanVariance].Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 14);

        CD3DX12_ROOT_PARAMETER rootParameters[Slot::Count];
        rootParameters[Slot::Output].InitAsDescriptorTable(1, &ranges[Slot::Output]);
        rootParameters[Slot::GBufferResources].InitAsDescriptorTable(1, &ranges[Slot::GBufferResources]);
        rootParameters[Slot::AO].InitAsDescriptorTable(1, &ranges[Slot::AO]);
        rootParameters[Slot::AORayHitDistance].InitAsDescriptorTable(1, &ranges[Slot::AORayHitDistance]);
        rootParameters[Slot::FrameAge].InitAsDescriptorTable(1, &ranges[Slot::FrameAge]);
        rootParameters[Slot::Color].InitAsDescriptorTable(1, &ranges[Slot::Color]);
        rootParameters[Slot::AOSurfaceAlbedo].InitAsDescriptorTable(1, &ranges[Slot::AOSurfaceAlbedo]);
        rootParameters[Slot::Variance].InitAsDescriptorTable(1, &ranges[Slot::Variance]);
        rootParameters[Slot::LocalMeanVariance].InitAsDescriptorTable(1, &ranges[Slot::LocalMeanVariance]);
        rootParameters[Slot::MaterialBuffer].InitAsShaderResourceView(7);
        rootParameters[Slot::ConstantBuffer].InitAsConstantBufferView(0);


        CD3DX12_ROOT_SIGNATURE_DESC rootSignatureDesc(ARRAYSIZE(rootParameters), rootParameters);
        SerializeAndCreateRootSignature(device, rootSignatureDesc, &m_computeRootSigs[CSType::ComposeRenderPassesCS], L"Root signature: ComposeRenderPassesCS");
    }

    // Create shader resources
    {
        m_csComposeRenderPassesCB.Create(device, FrameCount, L"Constant Buffer: ComposeRenderPassesCS");
    }

    // Create compute pipeline state.
    {
        D3D12_COMPUTE_PIPELINE_STATE_DESC descComputePSO = {};
        descComputePSO.pRootSignature = m_computeRootSigs[CSType::ComposeRenderPassesCS].Get();
        descComputePSO.CS = CD3DX12_SHADER_BYTECODE((void*)g_pCompositionCS, ARRAYSIZE(g_pCompositionCS));

        ThrowIfFailed(device->CreateComputePipelineState(&descComputePSO, IID_PPV_ARGS(&m_computePSOs[CSType::ComposeRenderPassesCS])));
        m_computePSOs[CSType::ComposeRenderPassesCS]->SetName(L"PSO: ComposeRenderPassesCS");
    }
}



// Upsample quarter resources
void Composition::UpsampleResourcesForRenderComposePass(
    Pathtracer& pathtracer,
    RTAO& rtao,
    Denoiser& denoiser,
    UINT GBufferWidth, 
    UINT GBufferHeight)
{
    auto commandList = m_deviceResources->GetCommandList();
    GpuResource* inputLowResValueResource = nullptr;
    GpuResource* outputHiResValueResource = nullptr;
    wstring passName;
    GpuKernels::UpsampleBilateralFilter::FilterType filterType = GpuKernels::UpsampleBilateralFilter::Filter2x2R;

    switch (Composition_Args::CompositionMode)
    {
        // ToDo Cleanup
    case CompositionType::PBRShading:
    case CompositionType::AmbientOcclusionOnly_Denoised:
    case CompositionType::AmbientOcclusionOnly_TemporallySupersampled:
    case CompositionType::AmbientOcclusionOnly_RawOneFrame:
    {
        passName = L"Upsample AO";
        outputHiResValueResource = &m_upsampledAOValueResource;

        if (Composition_Args::CompositionMode == CompositionType::AmbientOcclusionOnly_RawOneFrame)
        {
            inputLowResValueResource = &rtao.AOResources()[AOResource::Coefficient];
        }
        else
        {
            inputLowResValueResource = &denoiser.m_temporalAOCoefficient[denoiser.m_temporalCacheCurrentFrameTemporalAOCoefficientResourceIndex];
        }
        break;
    }
    case CompositionType::RTAOHitDistance:
    {
        passName = L"Upsample AO ray hit distance";
        inputLowResValueResource = &denoiser.m_temporalCache[denoiser.m_temporalCacheCurrentFrameResourceIndex][TemporalSupersampling::RayHitDistance];
        outputHiResValueResource = &m_upsampledAORayHitDistanceResource;
        break;
    }
    case CompositionType::AmbientOcclusionVariance:
    {
        passName = L"Upsample AO variance";
        inputLowResValueResource = Denoiser_Args::Denoising_UseSmoothedVariance ? &denoiser.m_varianceResources[AOVarianceResource::Smoothed] : &denoiser.m_varianceResources[AOVarianceResource::Raw];
        outputHiResValueResource = &m_upsampledVarianceResource;
        break;
    }
    case CompositionType::AmbientOcclusionLocalVariance:
    {
        passName = L"Upsample AO local variance";
        filterType = GpuKernels::UpsampleBilateralFilter::Filter2x2RG;
        inputLowResValueResource = Denoiser_Args::Denoising_UseSmoothedVariance ? &denoiser.m_localMeanVarianceResources[AOVarianceResource::Smoothed] : &denoiser.m_localMeanVarianceResources[AOVarianceResource::Raw];
        outputHiResValueResource = &m_upsampledLocalMeanVarianceResource;
        break;
    }
    default:
        break;
    }

    if (inputLowResValueResource)
    {
        auto& GBufferQuarterResResources = pathtracer.GBufferResources(true);
        auto& GBufferResources = pathtracer.GBufferResources(false);

        BilateralUpsample(
            GBufferWidth,
            GBufferHeight,
            filterType,
            inputLowResValueResource->gpuDescriptorReadAccess,
            GBufferQuarterResResources[GBufferResource::SurfaceNormalDepth].gpuDescriptorReadAccess,
            GBufferResources[GBufferResource::SurfaceNormalDepth].gpuDescriptorReadAccess,
            GBufferResources[GBufferResource::PartialDepthDerivatives].gpuDescriptorReadAccess,
            outputHiResValueResource,
            passName.c_str());
    }
}



// ToDo standardize naming AO vs AmbientOcclusion
void Composition::BilateralUpsample(
    UINT hiResWidth,
    UINT hiResHeight,
    GpuKernels::UpsampleBilateralFilter::FilterType filterType,
    const D3D12_GPU_DESCRIPTOR_HANDLE& inputLowResValueResourceHandle,
    const D3D12_GPU_DESCRIPTOR_HANDLE& inputLowResNormalDepthResourceHandle,
    const D3D12_GPU_DESCRIPTOR_HANDLE& inputHiResNormalDepthResourceHandle,
    const D3D12_GPU_DESCRIPTOR_HANDLE& inputHiResPartialDepthDerivativesResourceHandle,
    GpuResource* outputHiResValueResource,
    LPCWCHAR passName)
{
    auto commandList = m_deviceResources->GetCommandList();
    auto resourceStateTracker = m_deviceResources->GetGpuResourceStateTracker();

    ScopedTimer _prof(passName, commandList);

    resourceStateTracker->TransitionResource(outputHiResValueResource, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    resourceStateTracker->FlushResourceBarriers();
    m_upsampleBilateralFilterKernel.Run(
        commandList,
        hiResWidth,
        hiResHeight,
        filterType,
        m_cbvSrvUavHeap->GetHeap(),
        inputLowResValueResourceHandle,
        inputLowResNormalDepthResourceHandle,
        inputHiResNormalDepthResourceHandle,
        inputHiResPartialDepthDerivativesResourceHandle,
        outputHiResValueResource->gpuDescriptorWriteAccess,
        Composition_Args::UpsamplingUseBilinearWeights,
        Composition_Args::UpsamplingUseDepthWeights,
        Composition_Args::UpsamplingUseNormalWeights,
        Composition_Args::UpsamplingUseDynamicDepthThreshold
    );

    resourceStateTracker->TransitionResource(outputHiResValueResource, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
}


// ToDo remove?
void Composition::RenderRNGVisualizations()
{
#if 0
    auto device = m_deviceResources->GetD3DDevice();
    auto commandList = m_deviceResources->GetCommandList();
    auto frameIndex = m_deviceResources->GetCurrentFrameIndex();

    // Update constant buffer.
    XMUINT2 rngWindowSize(256, 256);
    {
        m_csHemisphereVisualizationCB->dispatchDimensions = rngWindowSize;

        static UINT seed = 0;
        UINT NumFramesPerIter = 400;
        static UINT frameID = NumFramesPerIter * 4;
        m_csHemisphereVisualizationCB->numSamplesToShow = m_randomSampler.NumSamples();// (frameID++ / NumFramesPerIter) % m_randomSampler.NumSamples();
        m_csHemisphereVisualizationCB->sampleSetBase = ((seed++ / NumFramesPerIter) % m_randomSampler.NumSampleSets()) * m_randomSampler.NumSamples();
        m_csHemisphereVisualizationCB->stratums = XMUINT2(static_cast<UINT>(sqrt(m_randomSampler.NumSamples())),
            static_cast<UINT>(sqrt(m_randomSampler.NumSamples())));
        m_csHemisphereVisualizationCB->grid = XMUINT2(m_randomSampler.NumSamples(), m_randomSampler.NumSamples());
        m_csHemisphereVisualizationCB->uavOffset = XMUINT2(0 /*ToDo remove m_renderingWidth - rngWindowSize.x*/, m_renderingHeight - rngWindowSize.y);
        m_csHemisphereVisualizationCB->numSamples = m_randomSampler.NumSamples();
        m_csHemisphereVisualizationCB->numSampleSets = m_randomSampler.NumSampleSets();
    }

    // Copy dynamic buffers to GPU
    {
        m_csHemisphereVisualizationCB.CopyStagingToGpu(frameIndex);
        m_samplesGPUBuffer.CopyStagingToGpu(frameIndex);
    }

    // Set pipeline state.
    {
        using namespace ComputeShader::RootSignature::HemisphereSampleSetVisualization;

        commandList->SetDescriptorHeaps(1, m_cbvSrvUavHeap->GetAddressOf());
        commandList->SetComputeRootSignature(m_computeRootSigs[CSType::HemisphereSampleSetVisualization].Get());
        commandList->SetPipelineState(m_computePSOs[CSType::HemisphereSampleSetVisualization].Get());

        commandList->SetComputeRootConstantBufferView(Slot::SceneConstant, m_csHemisphereVisualizationCB.GpuVirtualAddress(frameIndex));
        commandList->SetComputeRootShaderResourceView(Slot::SampleBuffers, m_samplesGPUBuffer.GpuVirtualAddress(frameIndex));
        commandList->SetComputeRootDescriptorTable(Slot::Output, m_raytracingOutput.gpuDescriptorWriteAccess);
    }

    // Dispatch.
    resourceStateTracker->FlushResourceBarriers();
    commandList->Dispatch(rngWindowSize.x, rngWindowSize.y, 1);
#endif
}


void Composition::CreateSamplesRNGVisualization()
{
#if 0
    auto device = m_deviceResources->GetD3DDevice();
    auto FrameCount = m_deviceResources->GetBackBufferCount();

    UINT samplesPerSet = m_sppAO * Composition_Args::AOSampleSetDistributedAcrossPixels * Composition_Args::AOSampleSetDistributedAcrossPixels;
    UINT NumSampleSets = 83;
    m_randomSampler.Reset(samplesPerSet, NumSampleSets, Samplers::HemisphereDistribution::Cosine);

    // Create root signature.
    {
        using namespace ComputeShader::RootSignature::HemisphereSampleSetVisualization;

        CD3DX12_DESCRIPTOR_RANGE ranges[1]; // Perfomance TIP: Order from most frequent to least frequent.
        ranges[0].Init(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0);  // 1 output texture

        CD3DX12_ROOT_PARAMETER rootParameters[Slot::Count];
        rootParameters[Slot::Output].InitAsDescriptorTable(1, &ranges[0]);
        rootParameters[Slot::SampleBuffers].InitAsShaderResourceView(1);
        rootParameters[Slot::SceneConstant].InitAsConstantBufferView(0);

        CD3DX12_ROOT_SIGNATURE_DESC rootSignatureDesc(ARRAYSIZE(rootParameters), rootParameters);
        SerializeAndCreateRootSignature(device, rootSignatureDesc, &m_computeRootSigs[CSType::HemisphereSampleSetVisualization], L"Root signature: CS hemisphere sample set visualization");
    }

    // Create compute pipeline state.
    {
        D3D12_COMPUTE_PIPELINE_STATE_DESC descComputePSO = {};
        descComputePSO.pRootSignature = m_computeRootSigs[CSType::HemisphereSampleSetVisualization].Get();
        descComputePSO.CS = CD3DX12_SHADER_BYTECODE((void*)g_pRNGVisualizerCS, ARRAYSIZE(g_pRNGVisualizerCS));

        ThrowIfFailed(device->CreateComputePipelineState(&descComputePSO, IID_PPV_ARGS(&m_computePSOs[CSType::HemisphereSampleSetVisualization])));
        m_computePSOs[CSType::HemisphereSampleSetVisualization]->SetName(L"PSO: CS hemisphere sample set visualization");
    }


    // Create shader resources
    {
        // ToDo rename GPU from resource names?
        m_csHemisphereVisualizationCB.Create(device, FrameCount, L"GPU CB: RNG");
        m_samplesGPUBuffer.Create(device, m_randomSampler.NumSamples() * m_randomSampler.NumSampleSets(), FrameCount, L"GPU buffer: Random unit square samples");
        m_hemisphereSamplesGPUBuffer.Create(device, m_randomSampler.NumSamples() * m_randomSampler.NumSampleSets(), FrameCount, L"GPU buffer: Random hemisphere samples");

        for (UINT i = 0; i < m_randomSampler.NumSamples() * m_randomSampler.NumSampleSets(); i++)
        {
            //sample.value = m_randomSampler.GetSample2D();
            XMFLOAT3 p = m_randomSampler.GetHemisphereSample3D();
            // Convert [-1,1] to [0,1].
            m_samplesGPUBuffer[i].value = XMFLOAT2(p.x * 0.5f + 0.5f, p.y * 0.5f + 0.5f);
            m_hemisphereSamplesGPUBuffer[i].value = p;
        }
    }
#endif
}



// Composite results from multiple passed into a final image.
void Composition::Render(
    GpuResource* outputResource,
    Scene& scene,
    Pathtracer& pathtracer, 
    RTAO& rtao,
    Denoiser& denoiser,
    UINT GBufferWidth, 
    UINT GBufferHeight)
{
    auto commandList = m_deviceResources->GetCommandList();
    auto resourceStateTracker = m_deviceResources->GetGpuResourceStateTracker();
    auto frameIndex = m_deviceResources->GetCurrentFrameIndex();
    auto& GBufferResources = pathtracer.GBufferResources();

    ScopedTimer _prof(L"ComposeRenderPassesCS", commandList);

    if (RTAO_Args::QuarterResAO)
    {
        UpsampleResourcesForRenderComposePass(pathtracer, rtao, denoiser, GBufferWidth, GBufferHeight);
    }



    // Update constant buffer.
    {
        m_csComposeRenderPassesCB->isAOEnabled = Composition_Args::AOEnabled;
        m_csComposeRenderPassesCB->compositionType = static_cast<CompositionType>(static_cast<UINT>(Composition_Args::CompositionMode));
        m_csComposeRenderPassesCB->defaultAmbientIntensity = Pathtracer_Args::DefaultAmbientIntensity;

        m_csComposeRenderPassesCB->variance_visualizeStdDeviation = Composition_Args::Compose_VarianceVisualizeStdDeviation;
        m_csComposeRenderPassesCB->variance_scale = Composition_Args::Compose_VarianceScale;
        m_csComposeRenderPassesCB->RTAO_MaxRayHitDistance = rtao.MaxRayHitTime();
        m_csComposeRenderPassesCB.CopyStagingToGpu(frameIndex);
    }

    // ToDo cleanup

    // Set pipeline state.
    {
        using namespace ComputeShader::RootSignature::ComposeRenderPassesCS;

        GpuResource* TemporalResources = denoiser.m_temporalCache[denoiser.m_temporalCacheCurrentFrameResourceIndex];
        GpuResource* VarianceResource = Denoiser_Args::Denoising_UseSmoothedVariance ? &denoiser.m_varianceResources[AOVarianceResource::Smoothed] : &denoiser.m_varianceResources[AOVarianceResource::Raw];
        GpuResource* LocalMeanVarianceResource = &denoiser.m_localMeanVarianceResources[AOVarianceResource::Raw];
        GpuResource* AORayHitDistance = &denoiser.m_temporalCache[denoiser.m_temporalCacheCurrentFrameResourceIndex][TemporalSupersampling::RayHitDistance];

        // ToDo
        GpuResource* AOResource = 
            Composition_Args::CompositionMode == CompositionType::AmbientOcclusionOnly_Denoised
            ? &denoiser.m_temporalAOCoefficient[denoiser.m_temporalCacheCurrentFrameTemporalAOCoefficientResourceIndex]
            : AOResource = &rtao.AOResources()[AOResource::Coefficient];
        GpuResource* TrppResource = &TemporalResources[TemporalSupersampling::FrameAge];

        if (RTAO_Args::QuarterResAO)
        {
            AOResource = &m_upsampledAOValueResource;
            TrppResource = &m_upsampledTrppResource;
            VarianceResource = &m_upsampledVarianceResource;
            LocalMeanVarianceResource = &m_upsampledLocalMeanVarianceResource;
            AORayHitDistance = &m_upsampledAORayHitDistanceResource;
        }

        commandList->SetDescriptorHeaps(1, m_cbvSrvUavHeap->GetAddressOf());
        commandList->SetComputeRootSignature(m_computeRootSigs[CSType::ComposeRenderPassesCS].Get());
        commandList->SetPipelineState(m_computePSOs[CSType::ComposeRenderPassesCS].Get());

        // Bind outputs.
        commandList->SetComputeRootDescriptorTable(Slot::Output, outputResource->gpuDescriptorWriteAccess);

        // Bind inputs.
        commandList->SetComputeRootDescriptorTable(Slot::GBufferResources, GBufferResources[0].gpuDescriptorReadAccess);
        commandList->SetComputeRootDescriptorTable(Slot::AO, AOResource->gpuDescriptorReadAccess);
        commandList->SetComputeRootShaderResourceView(Slot::MaterialBuffer, scene.m_materialBuffer.GpuVirtualAddress());
        commandList->SetComputeRootConstantBufferView(Slot::ConstantBuffer, m_csComposeRenderPassesCB.GpuVirtualAddress(frameIndex));
        commandList->SetComputeRootDescriptorTable(Slot::Variance, VarianceResource->gpuDescriptorReadAccess);
        commandList->SetComputeRootDescriptorTable(Slot::LocalMeanVariance, LocalMeanVarianceResource->gpuDescriptorReadAccess);

        commandList->SetComputeRootDescriptorTable(Slot::AORayHitDistance, AORayHitDistance->gpuDescriptorReadAccess);
        commandList->SetComputeRootDescriptorTable(Slot::Color, GBufferResources[GBufferResource::Color].gpuDescriptorReadAccess);
        commandList->SetComputeRootDescriptorTable(Slot::AOSurfaceAlbedo, GBufferResources[GBufferResource::AOSurfaceAlbedo].gpuDescriptorReadAccess);


        commandList->SetComputeRootDescriptorTable(Slot::FrameAge, TrppResource->gpuDescriptorReadAccess);
    }

    // Dispatch.
    XMUINT2 groupSize(CeilDivide(GBufferWidth, DefaultComputeShaderParams::ThreadGroup::Width), CeilDivide(GBufferHeight, DefaultComputeShaderParams::ThreadGroup::Height));

    resourceStateTracker->FlushResourceBarriers();
    commandList->Dispatch(groupSize.x, groupSize.y, 1);
}

