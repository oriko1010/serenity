/*
 * Copyright (c) 2020, Andreas Kling <kling@serenityos.org>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice, this
 *    list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 * CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#pragma once

#include <AK/RefCounted.h>
#include <LibGfx/AffineTransform.h>
#include <LibGfx/Color.h>
#include <LibGfx/Forward.h>
#include <LibWeb/Bindings/Wrappable.h>

namespace Web {

class CanvasRenderingContext2D
    : public RefCounted<CanvasRenderingContext2D>
    , public Bindings::Wrappable {

    AK_MAKE_NONCOPYABLE(CanvasRenderingContext2D);
    AK_MAKE_NONMOVABLE(CanvasRenderingContext2D);

public:
    using WrapperType = Bindings::CanvasRenderingContext2DWrapper;

    static NonnullRefPtr<CanvasRenderingContext2D> create(HTMLCanvasElement& element) { return adopt(*new CanvasRenderingContext2D(element)); }
    ~CanvasRenderingContext2D();

    void set_fill_style(String);
    String fill_style() const;

    void set_stroke_style(String);
    String stroke_style() const;

    void fill_rect(float x, float y, float width, float height);
    void stroke_rect(float x, float y, float width, float height);

    void draw_image(const HTMLImageElement&, float x, float y);

    void scale(float sx, float sy);
    void translate(float x, float y);

private:
    explicit CanvasRenderingContext2D(HTMLCanvasElement&);

    void did_draw(const Gfx::FloatRect&);

    OwnPtr<Gfx::Painter> painter();

    WeakPtr<HTMLCanvasElement> m_element;

    Gfx::AffineTransform m_transform;
    Gfx::Color m_fill_style;
    Gfx::Color m_stroke_style;
};

}
