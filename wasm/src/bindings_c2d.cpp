#include "rive/rive_types.hpp"

#ifdef RIVE_CANVAS_2D_RENDERER

#include "rive/factory.hpp"
#include "rive/renderer.hpp"
#include "rive/math/path_types.hpp"
#include "utils/factory_utils.hpp"

#include "rive/assets/file_asset.hpp"
#include "rive/assets/image_asset.hpp"

#include "skia_imports/include/private/SkVx.h"
#include "js_alignment.hpp"

#include <emscripten.h>
#include <emscripten/bind.h>
#include <emscripten/val.h>
#include <stdint.h>
#include <stdio.h>
#include <string>
#include <vector>

using namespace emscripten;

// Computes the post-transform bounding box of an array of points in high performance WASM SIMD.
static std::array<float, 4> bbox(const float m[6], const float* vertexData, int numVertexFloats)
{
    using float2 = skvx::Vec<2, float>;
    using float4 = skvx::Vec<4, float>;

    assert(numVertexFloats > 0);
    assert(numVertexFloats % 2 == 0); // numVertexFloats must be even -- 2 floats per vertex.

    float4 scale = {m[0], m[3], m[0], m[3]};
    float4 skew = {m[2], m[1], m[2], m[1]};
    float2 translate = {m[4], m[5]};

    // Compute two partial bounding boxes in parallel lanes of float4. Defer the translation until
    // after min/max reduction.
    float4 partialTopLefts, partialBotRights;
    float4 v0;
    int i;
    // TODO: could 128-bit alignment on loads impact our speed in WASM?
    if (!(numVertexFloats & 3))
    {
        // Even number of vertices -- number of floats is divisible by 4. Load 2 vertices initially.
        v0 = float4::Load(vertexData);
        i = 4;
    }
    else
    {
        // Odd number of vertices. Load 1 vertex initially so the rest will be divisible by 4.
        v0 = float2::Load(vertexData).xyxy();
        i = 2;
    }
    partialTopLefts = partialBotRights = v0 * scale + v0.yxwz() * skew;
    // Crunch the remaining vertices in float4 SIMD.
    for (; i < numVertexFloats; i += 4)
    {
        float4 v = float4::Load(vertexData + i);
        v = v * scale + v.yxwz() * skew;
        partialTopLefts = min(partialTopLefts, v);
        partialBotRights = max(partialBotRights, v);
    }
    assert(i == numVertexFloats);

    // Merge the two parallel bounding boxes into one complete, translated, integer bounding box.
    float2 topLeft = floor(min(partialTopLefts.lo, partialTopLefts.hi) + translate);
    float2 botRight = ceil(max(partialBotRights.lo, partialBotRights.hi) + translate);
    return {topLeft.x(), topLeft.y(), botRight.x(), botRight.y()};
}

class RendererWrapper : public wrapper<rive::Renderer>
{
public:
    EMSCRIPTEN_WRAPPER(RendererWrapper);

    void save() override { call<void>("save"); }

    void restore() override { call<void>("restore"); }

    void transform(const rive::Mat2D& transform) override
    {
        call<void>("transform",
                   transform.xx(),
                   transform.xy(),
                   transform.yx(),
                   transform.yy(),
                   transform.tx(),
                   transform.ty());
    }

    void align(rive::Fit fit,
               JsAlignment alignment,
               const rive::AABB& foo,
               const rive::AABB& bar,
               const float scaleFactor = 1.0f)
    {
        transform(computeAlignment(fit, convertAlignment(alignment), foo, bar, scaleFactor));
    }

    void drawPath(rive::RenderPath* path, rive::RenderPaint* paint) override
    {
        call<void>("_drawPath", path, paint);
    }

    void clipPath(rive::RenderPath* path) override { call<void>("_clipPath", path); }

    void drawImage(const rive::RenderImage* image,
                   const rive::ImageSampler options,
                   rive::BlendMode value,
                   float opacity) override
    {
        call<void>("_drawRiveImage", image, options, value, opacity);
    }

    void drawImageMesh(const rive::RenderImage* image,
                       const rive::ImageSampler options,
                       rive::rcp<rive::RenderBuffer> vertices_f32,
                       rive::rcp<rive::RenderBuffer> uvCoords_f32,
                       rive::rcp<rive::RenderBuffer> indices_u16,
                       uint32_t vertexCount,
                       uint32_t indexCount,
                       rive::BlendMode value,
                       float opacity) override
    {
        LITE_RTTI_CAST_OR_RETURN(vtx, rive::DataRenderBuffer*, vertices_f32.get());
        LITE_RTTI_CAST_OR_RETURN(uv, rive::DataRenderBuffer*, uvCoords_f32.get());
        LITE_RTTI_CAST_OR_RETURN(indices, rive::DataRenderBuffer*, indices_u16.get());

        uint32_t f32Count = vertexCount * 2;
        assert(vtx->sizeInBytes() == f32Count * sizeof(float));
        assert(uv->sizeInBytes() == f32Count * sizeof(float));
        assert(indices->sizeInBytes() == indexCount * sizeof(uint16_t));

        if (f32Count == 0 || indexCount == 0)
        {
            return;
        }

        emscripten::val uvJS{emscripten::typed_memory_view(f32Count, uv->f32s())};
        emscripten::val vtxJS{emscripten::typed_memory_view(f32Count, vtx->f32s())};
        emscripten::val indicesJS{emscripten::typed_memory_view(indexCount, indices->u16s())};

        // Compute the mesh's bounding box.
        float m[6];
        emscripten::val mJS{emscripten::typed_memory_view(6, m)};
        call<void>("_getMatrix", mJS);
        auto [l, t, r, b] = bbox(m, vtx->f32s(), f32Count);

        call<void>("_drawImageMesh",
                   image,
                   options,
                   value,
                   opacity,
                   vtxJS,
                   uvJS,
                   indicesJS,
                   l,
                   t,
                   r,
                   b);
    }
};

class RenderPathWrapper : public wrapper<rive::RenderPath>
{
public:
    EMSCRIPTEN_WRAPPER(RenderPathWrapper);

    void rewind() override { call<void>("rewind"); }

    void addRawPath(const rive::RawPath& path) override
    {
        // It might be faster to do this on the JS side, and just pass up the arrays...
        // for now, we do it one segment at a time (each turns into an up-call to JS)
        const rive::Vec2D* pts = path.points().data();
        for (auto v : path.verbs())
        {
            switch ((rive::PathVerb)v)
            {
                case rive::PathVerb::move:
                    move(*pts++);
                    break;
                case rive::PathVerb::line:
                    line(*pts++);
                    break;
                case rive::PathVerb::cubic:
                    cubic(pts[0], pts[1], pts[2]);
                    pts += 3;
                    break;
                case rive::PathVerb::close:
                    close();
                    break;
                default:
                    assert(false); // unexpected verb
            }
        }
        assert(pts - path.points().data() == path.points().size());
    }

    void addRenderPath(rive::RenderPath* path, const rive::Mat2D& transform) override
    {
        float xx = transform.xx();
        float xy = transform.xy();
        float yx = transform.yx();
        float yy = transform.yy();
        float tx = transform.tx();
        float ty = transform.ty();
        call<void>("addPath", path, xx, xy, yx, yy, tx, ty);
    }
    void fillRule(rive::FillRule value) override { call<void>("fillRule", value); }

    void moveTo(float x, float y) override { call<void>("moveTo", x, y); }
    void lineTo(float x, float y) override { call<void>("lineTo", x, y); }
    void cubicTo(float ox, float oy, float ix, float iy, float x, float y) override
    {
        call<void>("cubicTo", ox, oy, ix, iy, x, y);
    }
    void close() override { call<void>("close"); }
};

class RenderPaintWrapper;
class GradientShader : public rive::RenderShader
{
private:
    std::vector<float> m_Stops;
    std::vector<rive::ColorInt> m_Colors;

public:
    GradientShader(const rive::ColorInt colors[], const float stops[], int count) :
        m_Stops(stops, stops + count), m_Colors(colors, colors + count)
    {}

    void passStopsToJS(const RenderPaintWrapper& wrapper);

    virtual void passToJS(const RenderPaintWrapper& wrapper) = 0;
};

class LinearGradientShader : public GradientShader
{
private:
    float m_StartX;
    float m_StartY;
    float m_EndX;
    float m_EndY;

public:
    LinearGradientShader(const rive::ColorInt colors[],
                         const float stops[],
                         int count,
                         float sx,
                         float sy,
                         float ex,
                         float ey) :
        GradientShader(colors, stops, count), m_StartX(sx), m_StartY(sy), m_EndX(ex), m_EndY(ey)
    {}

    void passToJS(const RenderPaintWrapper& wrapper) override;
};

class RadialGradientShader : public GradientShader
{
private:
    float m_CenterX;
    float m_CenterY;
    float m_Radius;

public:
    RadialGradientShader(const rive::ColorInt colors[],
                         const float stops[],
                         int count,
                         float cx,
                         float cy,
                         float r) :
        GradientShader(colors, stops, count), m_CenterX(cx), m_CenterY(cy), m_Radius(r)
    {}

    void passToJS(const RenderPaintWrapper& wrapper) override;
};

class RenderPaintWrapper : public wrapper<rive::RenderPaint>
{
public:
    EMSCRIPTEN_WRAPPER(RenderPaintWrapper);

    void color(unsigned int value) override { call<void>("color", value); }
    void thickness(float value) override { call<void>("thickness", value); }
    void join(rive::StrokeJoin value) override { call<void>("join", value); }
    void cap(rive::StrokeCap value) override { call<void>("cap", value); }
    void blendMode(rive::BlendMode value) override { call<void>("blendMode", value); }

    void style(rive::RenderPaintStyle value) override { call<void>("style", value); }

    void shader(rive::rcp<rive::RenderShader> shader) override
    {
        if (shader == nullptr)
        {
            call<void>("clearGradient");
            return;
        }
        static_cast<GradientShader*>(shader.get())->passToJS(*this);
    }
    void invalidateStroke() override {}
};

void GradientShader::passStopsToJS(const RenderPaintWrapper& wrapper)
{
    // Consider passing in a bulk op encoding into a single array.
    for (std::size_t i = 0; i < m_Stops.size(); i++)
    {
        wrapper.call<void>("addStop", m_Colors[i], m_Stops[i]);
    }
}

void LinearGradientShader::passToJS(const RenderPaintWrapper& wrapper)
{
    wrapper.call<void>("linearGradient", m_StartX, m_StartY, m_EndX, m_EndY);
    passStopsToJS(wrapper);
}

void RadialGradientShader::passToJS(const RenderPaintWrapper& wrapper)
{
    wrapper.call<void>("radialGradient", m_CenterX, m_CenterY, m_CenterX + m_Radius, m_CenterY);
    passStopsToJS(wrapper);
}

class RenderImageWrapper : public wrapper<rive::RenderImage>
{
public:
    EMSCRIPTEN_WRAPPER(RenderImageWrapper);

    bool decode(rive::Span<const uint8_t> bytes)
    {
        emscripten::val byteArray =
            emscripten::val(emscripten::typed_memory_view(bytes.size(), bytes.data()));
        call<val>("decode", byteArray);
        return true;
    }

    void size(int width, int height)
    {
        m_Width = width;
        m_Height = height;
    }
    void unref() { rive::RenderImage::unref(); }
};

namespace rive
{

class C2DFactory : public Factory
{
    rcp<RenderBuffer> makeRenderBuffer(RenderBufferType type,
                                       RenderBufferFlags flags,
                                       size_t sizeInBytes) override
    {
        return make_rcp<DataRenderBuffer>(type, flags, sizeInBytes);
    }

    rcp<RenderShader> makeLinearGradient(float sx,
                                         float sy,
                                         float ex,
                                         float ey,
                                         const ColorInt colors[], // [count]
                                         const float stops[],     // [count]
                                         size_t count) override
    {
        return rcp<RenderShader>(new LinearGradientShader(colors, stops, count, sx, sy, ex, ey));
    }
    rcp<RenderShader> makeRadialGradient(float cx,
                                         float cy,
                                         float radius,
                                         const ColorInt colors[], // [count]
                                         const float stops[],     // [count]
                                         size_t count) override
    {
        return rcp<RenderShader>(new RadialGradientShader(colors, stops, count, cx, cy, radius));
    }

    rcp<RenderPath> makeRenderPath(RawPath& path, FillRule fr) override
    {
        val renderPath = val::module_property("renderFactory").call<val>("makeRenderPath");
        auto ptr = renderPath.as<RenderPath*>(allow_raw_pointers());
        ptr->addRawPath(path);

        ptr->fillRule(fr);

        return rcp(ptr); // Adopt this ref without increasing the refcount.
    }

    rcp<RenderPath> makeEmptyRenderPath() override
    {
        val renderPath = val::module_property("renderFactory").call<val>("makeRenderPath");
        auto ptr = renderPath.as<RenderPath*>(allow_raw_pointers());
        return rcp(ptr); // Adopt this ref without increasing the refcount.
    }

    rcp<RenderPaint> makeRenderPaint() override
    {
        val renderPaint = val::module_property("renderFactory").call<val>("makeRenderPaint");
        auto ptr = renderPaint.as<RenderPaint*>(allow_raw_pointers());
        return rcp(ptr); // Adopt this ref without increasing the refcount.
    }

    rcp<RenderImage> decodeImage(Span<const uint8_t> bytes) override
    {
        // NOTE::
        // This path is only used for hostedImages & embedded images.
        // I think we should refactor this so everything follows the same path.

        // TODO: seems like we should change the constructor the the JS RenderImage to
        //       be passed the byteArray, and have it decode (or fail) right away.
        //       It could just return null to us for its object if it failed.
        //   ... that would avoid that tricky cast to RenderImageWrapper*

        val renderImage = val::module_property("renderFactory").call<val>("makeRenderImage");

        rcp<RenderImageWrapper> ptr =
            rcp(renderImage.as<RenderImageWrapper*>(allow_raw_pointers()));
        if (!ptr->decode(bytes))
        {
            // Question, what do we do when we end up here?
            //       safe_unref(ptr);
            //       ptr = nullptr;
        }

        return ptr;
    }
};

} // namespace rive

EMSCRIPTEN_BINDINGS(RiveWASM_C2D)
{
    class_<rive::Renderer>("Renderer")
        .function("save", &RendererWrapper::save, pure_virtual(), allow_raw_pointers())
        .function("restore", &RendererWrapper::restore, pure_virtual(), allow_raw_pointers())
        .function("transform", &RendererWrapper::transform, pure_virtual(), allow_raw_pointers())
        .function("drawPath", &RendererWrapper::drawPath, pure_virtual(), allow_raw_pointers())
        .function("clipPath", &RendererWrapper::clipPath, pure_virtual(), allow_raw_pointers())
        .function("align", &RendererWrapper::align, pure_virtual(), allow_raw_pointers())
        .allow_subclass<RendererWrapper>("RendererWrapper");

    class_<rive::RenderPath>("RenderPath")
        .function("rewind", &RenderPathWrapper::rewind, pure_virtual(), allow_raw_pointers())
        .function("addPath",
                  &RenderPathWrapper::addRenderPath,
                  pure_virtual(),
                  allow_raw_pointers())
        .function("fillRule", &RenderPathWrapper::fillRule, pure_virtual())
        .function("moveTo", &RenderPathWrapper::moveTo, pure_virtual(), allow_raw_pointers())
        .function("lineTo", &RenderPathWrapper::lineTo, pure_virtual(), allow_raw_pointers())
        .function("cubicTo", &RenderPathWrapper::cubicTo, pure_virtual(), allow_raw_pointers())
        .function("close", &RenderPathWrapper::close, pure_virtual(), allow_raw_pointers())
        .allow_subclass<RenderPathWrapper>("RenderPathWrapper");
    enum_<rive::RenderPaintStyle>("RenderPaintStyle")
        .value("fill", rive::RenderPaintStyle::fill)
        .value("stroke", rive::RenderPaintStyle::stroke);

    enum_<rive::FillRule>("FillRule")
        .value("nonZero", rive::FillRule::nonZero)
        .value("evenOdd", rive::FillRule::evenOdd)
        .value("clockwise", rive::FillRule::clockwise);

    enum_<rive::StrokeCap>("StrokeCap")
        .value("butt", rive::StrokeCap::butt)
        .value("round", rive::StrokeCap::round)
        .value("square", rive::StrokeCap::square);

    enum_<rive::StrokeJoin>("StrokeJoin")
        .value("miter", rive::StrokeJoin::miter)
        .value("round", rive::StrokeJoin::round)
        .value("bevel", rive::StrokeJoin::bevel);

    enum_<rive::BlendMode>("BlendMode")
        .value("srcOver", rive::BlendMode::srcOver)
        .value("screen", rive::BlendMode::screen)
        .value("overlay", rive::BlendMode::overlay)
        .value("darken", rive::BlendMode::darken)
        .value("lighten", rive::BlendMode::lighten)
        .value("colorDodge", rive::BlendMode::colorDodge)
        .value("colorBurn", rive::BlendMode::colorBurn)
        .value("hardLight", rive::BlendMode::hardLight)
        .value("softLight", rive::BlendMode::softLight)
        .value("difference", rive::BlendMode::difference)
        .value("exclusion", rive::BlendMode::exclusion)
        .value("multiply", rive::BlendMode::multiply)
        .value("hue", rive::BlendMode::hue)
        .value("saturation", rive::BlendMode::saturation)
        .value("color", rive::BlendMode::color)
        .value("luminosity", rive::BlendMode::luminosity);

    class_<rive::rcp<rive::RenderShader>>("RenderShader");

    class_<rive::RenderPaint>("RenderPaint")
        .function("color", &RenderPaintWrapper::color, pure_virtual(), allow_raw_pointers())

        .function("style", &RenderPaintWrapper::style, pure_virtual(), allow_raw_pointers())
        .function("thickness", &RenderPaintWrapper::thickness, pure_virtual(), allow_raw_pointers())
        .function("join", &RenderPaintWrapper::join, pure_virtual(), allow_raw_pointers())
        .function("cap", &RenderPaintWrapper::cap, pure_virtual(), allow_raw_pointers())
        .function("blendMode", &RenderPaintWrapper::blendMode, pure_virtual(), allow_raw_pointers())
        .function("shader", &RenderPaintWrapper::shader, pure_virtual(), allow_raw_pointers())
        .allow_subclass<RenderPaintWrapper>("RenderPaintWrapper");

    class_<rive::RenderImage>("RenderImage")
        .function("size", &RenderImageWrapper::size)
        .function("unref", &RenderImageWrapper::unref)
        .allow_subclass<RenderImageWrapper>("RenderImageWrapper");
}

static rive::C2DFactory gC2DFactory;
rive::Factory* jsFactory() { return &gC2DFactory; }

#endif // RIVE_CANVAS_2D_RENDERER
