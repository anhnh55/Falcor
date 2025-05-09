/***************************************************************************
 # Copyright (c) 2015-23, NVIDIA CORPORATION. All rights reserved.
 #
 # Redistribution and use in source and binary forms, with or without
 # modification, are permitted provided that the following conditions
 # are met:
 #  * Redistributions of source code must retain the above copyright
 #    notice, this list of conditions and the following disclaimer.
 #  * Redistributions in binary form must reproduce the above copyright
 #    notice, this list of conditions and the following disclaimer in the
 #    documentation and/or other materials provided with the distribution.
 #  * Neither the name of NVIDIA CORPORATION nor the names of its
 #    contributors may be used to endorse or promote products derived
 #    from this software without specific prior written permission.
 #
 # THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS "AS IS" AND ANY
 # EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 # IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 # PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT OWNER OR
 # CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 # EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 # PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 # PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 # OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 # (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 # OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 **************************************************************************/
#include "TestPhongModelPass.h"

extern "C" FALCOR_API_EXPORT void registerPlugin(Falcor::PluginRegistry& registry)
{
    registry.registerClass<RenderPass, TestPhongModelPass>();
}
namespace
{
const char kShaderFile[] = "RenderPasses/TestPhongModelPass/PhongShader.slang";

} // namespace

TestPhongModelPass::TestPhongModelPass(ref<Device> pDevice, const Properties& props) : RenderPass(pDevice)
{
    mpFbo = Fbo::create(mpDevice);
    Sampler::Desc samplerDesc;
    samplerDesc.setFilterMode(TextureFilteringMode::Linear, TextureFilteringMode::Linear, TextureFilteringMode::Linear);

    Sampler::Desc shadowSamplerDesc;
    shadowSamplerDesc.setFilterMode(TextureFilteringMode::Linear, TextureFilteringMode::Linear, TextureFilteringMode::Linear)
        .setReductionMode(TextureReductionMode::Comparison)
        .setComparisonFunc(ComparisonFunc::LessEqual);
    mpLinearSampler = mpDevice->createSampler(samplerDesc);
    mpShadowSampler = mpDevice->createSampler(shadowSamplerDesc);
}

Properties TestPhongModelPass::getProperties() const
{
    return {};
}

RenderPassReflection TestPhongModelPass::reflect(const CompileData& compileData)
{
    // Define the required resources here
    RenderPassReflection reflector;
    const uint2 sz = RenderPassHelpers::calculateIOSize(mOutputSizeSelection, mFixedOutputSize, compileData.defaultTexDims);
    reflector.addInput("shadowmap", "shadow map").flags(RenderPassReflection::Field::Flags::Optional);
    reflector.addInput("shadowTransform", "shadow transform matrix")
        .resourceType(RenderPassReflection::Field::Type::RawBuffer, sizeof(float4x4), 0, 0, 0, 0, 0)
        .flags(RenderPassReflection::Field::Flags::Optional);
    reflector.addOutput("output", "PhongModel view texture");
    // Add the required depth output. This always exists.
    reflector.addOutput("depth", "Depth buffer")
        .format(ResourceFormat::D32Float)
        .bindFlags(ResourceBindFlags::DepthStencil)
        .texture2D(sz.x, sz.y);
    return reflector;
}

void TestPhongModelPass::execute(RenderContext* pRenderContext, const RenderData& renderData)
{
    auto pTargetFbo = renderData.getTexture("output");
    const float4 clearColor(0, 0, 0, 1);
    mpFbo->attachColorTarget(pTargetFbo, 0);

    // Update frame dimension based on render pass output.
    auto pDepth = renderData.getTexture("depth");
    FALCOR_ASSERT(pDepth);
    // updateFrameDim(uint2(pDepth->getWidth(), pDepth->getHeight()));
    //  Clear depth buffer.
    pRenderContext->clearDsv(pDepth->getDSV().get(), 1.f, 0);
    mpFbo->attachDepthStencilTarget(pDepth);

    pRenderContext->clearFbo(mpFbo.get(), clearColor, 1.0f, 0, FboAttachmentType::Color);
    
    if (mpScene)
    {
        auto pShadowMap = renderData.getTexture("shadowmap");
        auto pShadowTransform = renderData.getResource("shadowTransform")->asBuffer();
        float4x4 mat = float4x4::identity();
        if (pShadowTransform)
        {
            pShadowTransform->getBlob(&mat[0][0], 0, sizeof(float4x4));
            pShadowTransform->setName("Shadow Transform");
        }
           

        auto var = mpVars->getRootVar();
        var["PerFrameCB"]["gShadowTransform"] = mat;
        var["gSampler"] = mpLinearSampler;
        var["gSamShadow"] = mpShadowSampler;
        if (pShadowMap)
        {
            var["gShadowMap"] = pShadowMap;
            pShadowMap->setName("Shadow Map");
        }

        mpScene->rasterize(pRenderContext, mpGraphicsState.get(), mpVars.get(), mpRasterState, mpRasterState);
    }
}

void TestPhongModelPass::renderUI(Gui::Widgets& widget) {}

void TestPhongModelPass::setScene(RenderContext* pRenderContext, const ref<Scene>& pScene)
{
    // Set new scene.
    mpScene = pScene;
    if (mpScene)
    {
        // program
        ProgramDesc desc;
        desc.addShaderModules(mpScene->getShaderModules());
        desc.addShaderLibrary(kShaderFile)
            .vsEntry("vsMain")  // Vertex shader entry point
            .psEntry("psMain"); // Pixel shader entry point;
        //desc.setShaderModel(ShaderModel::SM6_3);
        mpProgram = Program::create(mpDevice, desc, mpScene->getSceneDefines());
        mpVars = ProgramVars::create(mpDevice, mpProgram->getReflector());

        // rasterizer state
        RasterizerState::Desc rasterDesc;
        rasterDesc.setFillMode(RasterizerState::FillMode::Solid);
        rasterDesc.setCullMode(RasterizerState::CullMode::None);
        rasterDesc.setDepthBias(100000, 1.0f);
        mpRasterState = RasterizerState::create(rasterDesc);

        //default depth stencil state
        DepthStencilState::Desc dsDesc;
        //dsDesc.setDepthFunc(ComparisonFunc::Greater);
        ref<DepthStencilState> pDsState = DepthStencilState::create(dsDesc);

        mpGraphicsState = GraphicsState::create(mpDevice);
        mpGraphicsState->setProgram(mpProgram);
        mpGraphicsState->setRasterizerState(mpRasterState);
        mpGraphicsState->setFbo(mpFbo);
        mpGraphicsState->setDepthStencilState(pDsState);
    }
}
