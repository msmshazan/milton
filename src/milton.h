// milton.h
// (c) Copyright 2015 by Sergio Gonzalez


// Rename types for convenience
typedef int8_t      int8;
typedef uint8_t     uint8;
typedef int16_t     int16;
typedef uint16_t    uint16;
typedef int32_t     int32;
typedef uint32_t    uint32;
typedef int64_t     int64;
typedef uint64_t    uint64;
typedef int32_t     bool32;

#if defined(_MSC_VER)
#define true 1
#define false 0
#endif


#include <math.h>  // powf
#include <float.h>

#include "libserg/gl_helpers.h"
#include "vector.generated.h"  // Generated by metaprogram

#include "utils.h"
#include "color.h"


typedef struct Brush_s
{
    int32 radius;  // This should be replaced by a BrushType and some union containing brush info.
    v3f   color;
    float alpha;
} Brush;

#define LIMIT_STROKE_POINTS 1024
typedef struct Stroke_s
{
    Brush   brush;
    // TODO turn this into a deque??
    v2i     points[LIMIT_STROKE_POINTS];
    v2i     clipped_points[2 * LIMIT_STROKE_POINTS];  // Clipped points are in the form [ab bc cd df]
                                                        // That's why we double the space for them.
    int32   num_points;
    int32   num_clipped_points;
    Rect    bounds;
} Stroke;

typedef struct MiltonGLState_s
{
    GLuint quad_program;
    GLuint texture;
    GLuint quad_vao;
} MiltonGLState;

typedef struct MiltonState_s
{
    int32_t     full_width;             // Dimensions of the raster
    int32_t     full_height;
    uint8_t     bytes_per_pixel;
    uint8_t*    raster_buffer;
    size_t      raster_buffer_size;

    MiltonGLState* gl;

    ColorManagement cm;

    ColorPicker picker;

    Brush brush;
    int32 brush_size;  // In screen pixels

    bool32 canvas_blocked;  // When interacting with the UI.

    v2i screen_size;

    // Maps screen_size to a rectangle in our infinite canvas.
    int32 view_scale;

    v2i     last_point;  // Last input point. Used to determine area to update.
    Stroke  working_stroke;

    Stroke  strokes[4096];  // TODO: Create a deque to store arbitrary number of strokes.
    int32   num_strokes;

    // Heap
    Arena*      root_arena;         // Persistent memory.
    Arena*      transient_arena;    // Gets reset after every call to milton_update().

} MiltonState;

typedef struct MiltonInput_s
{
    bool32 full_refresh;
    bool32 reset;
    bool32 end_stroke;
    v2i* point;
    int scale;
} MiltonInput;

static void milton_gl_backend_draw(MiltonState* milton_state)
{
    MiltonGLState* gl = milton_state->gl;
    glTexImage2D(
            GL_TEXTURE_2D, 0, GL_RGBA,
            milton_state->screen_size.w, milton_state->screen_size.h,
            0, GL_RGBA, GL_UNSIGNED_BYTE, (GLvoid*)milton_state->raster_buffer);
    glUseProgram(gl->quad_program);
    glBindVertexArray(gl->quad_vao);
    GLCHK (glDrawArrays (GL_TRIANGLE_FAN, 0, 4) );
}

static void milton_gl_backend_init(MiltonState* milton_state)
{
    // Init quad program
    {
        const char* shader_contents[2];

        shader_contents[0] =
            "#version 330\n"
            "#extension GL_ARB_explicit_uniform_location : enable\n"
            "layout(location = 0) in vec2 position;\n"
            "\n"
            "out vec2 coord;\n"
            "\n"
            "void main()\n"
            "{\n"
            "   coord = (position + vec2(1,1))/2;\n"
            "   coord.y = 1 - coord.y;"
            "   // direct to clip space. must be in [-1, 1]^2\n"
            "   gl_Position = vec4(position, 0.0, 1.0);\n"
            "}\n";


        shader_contents[1] =
            "#version 330\n"
            "#extension GL_ARB_explicit_uniform_location : enable\n"
            "\n"
            "layout(location = 1) uniform sampler2D buffer;\n"
            "in vec2 coord;\n"
            "out vec4 out_color;\n"
            "\n"
            "vec3 sRGB_to_linear(vec3 rgb)\n"
            "{\n"
                "vec3 result = pow((rgb + vec3(0.055)) / vec3(1.055), vec3(2.4));\n"
                "return result;\n"
            "}\n"
            "void main(void)\n"
            "{\n"
            "   out_color = texture(buffer, coord);"
            // TODO: Why am I getting BGRA format?!?!?
            "   out_color = vec4(sRGB_to_linear(out_color.rgb), 1).bgra;"
            "}\n";

        GLuint shader_objects[2] = {0};
        for ( int i = 0; i < 2; ++i )
        {
            GLuint shader_type = (i == 0) ? GL_VERTEX_SHADER : GL_FRAGMENT_SHADER;
            shader_objects[i] = gl_compile_shader(shader_contents[i], shader_type);
        }
        milton_state->gl->quad_program = glCreateProgram();
        gl_link_program(milton_state->gl->quad_program, shader_objects, 2);

        glUseProgram(milton_state->gl->quad_program);
        glUniform1i(1, 0 /*GL_TEXTURE0*/);
    }

    // Create texture
    {
        GLCHK (glActiveTexture (GL_TEXTURE0) );
        // Create texture
        GLCHK (glGenTextures   (1, &milton_state->gl->texture));
        GLCHK (glBindTexture   (GL_TEXTURE_2D, milton_state->gl->texture));

        // Note for the future: These are needed.
        GLCHK (glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR));
        GLCHK (glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR));
        GLCHK (glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_BORDER));
        GLCHK (glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_BORDER));

        // Pass a null pointer, texture will be filled by opencl ray tracer
        GLCHK ( glTexImage2D(
                    GL_TEXTURE_2D, 0, GL_RGBA,
                    milton_state->screen_size.w, milton_state->screen_size.h,
                    0, GL_RGBA, GL_FLOAT, NULL) );
    }
    // Create quad
    {
        //const GLfloat u = 1.0f;
#define u -1.0f
        // full
        GLfloat vert_data[] =
        {
            -u, u,
            -u, -u,
            u, -u,
            u, u,
        };
#undef u
        GLCHK (glGenVertexArrays(1, &milton_state->gl->quad_vao));
        GLCHK (glBindVertexArray(milton_state->gl->quad_vao));

        GLuint vbo;
        GLCHK (glGenBuffers(1, &vbo));
        GLCHK (glBindBuffer(GL_ARRAY_BUFFER, vbo));

        GLCHK (glBufferData (GL_ARRAY_BUFFER, sizeof(vert_data), vert_data, GL_STATIC_DRAW));
        GLCHK (glEnableVertexAttribArray (0) );
        GLCHK (glVertexAttribPointer     (/*attrib location*/0,
                    /*size*/2, GL_FLOAT, /*normalize*/GL_FALSE, /*stride*/0, /*ptr*/0));
    }
}

#ifndef NDEBUG
static void milton_startup_tests()
{
    v3f rgb = hsv_to_rgb((v3f){ 0 });
    assert(
            rgb.r == 0 &&
            rgb.g == 0 &&
            rgb.b == 0
          );
    rgb = hsv_to_rgb((v3f){ 0, 0, 1.0 });
    assert(
            rgb.r == 1 &&
            rgb.g == 1 &&
            rgb.b == 1
          );
    rgb = hsv_to_rgb((v3f){ 120, 1.0f, 0.5f });
    assert(
            rgb.r == 0 &&
            rgb.g == 0.5 &&
            rgb.b == 0
          );
    rgb = hsv_to_rgb((v3f){ 0, 1.0f, 1.0f });
    assert(
            rgb.r == 1.0 &&
            rgb.g == 0 &&
            rgb.b == 0
          );
}
#endif

static void milton_init(MiltonState* milton_state)
{
#ifndef NDEBUG
    milton_startup_tests();
#endif
    // Allocate enough memory for the maximum possible supported resolution. As
    // of now, it seems like future 8k displays will adopt this resolution.
    milton_state->full_width      = 7680;
    milton_state->full_height     = 4320;
    milton_state->bytes_per_pixel = 4;
    milton_state->view_scale      = (1 << 12);
    milton_state->num_strokes     = 0;  // Working stroke is index 0

    milton_state->gl = arena_alloc_elem(milton_state->root_arena, MiltonGLState);

    color_init(&milton_state->cm);

    // Init picker
    {
        int32 bound_radius_px = 80;
        float wheel_half_width = 8;
        milton_state->picker.center = (v2i){ 120, 120 };
        milton_state->picker.bound_radius_px = bound_radius_px;
        milton_state->picker.wheel_half_width = wheel_half_width;
        milton_state->picker.wheel_radius = (float)bound_radius_px - 5.0f - wheel_half_width;
        milton_state->picker.hsv = (v3f){ 0.0f, 1.0f, 0.7f };
        Rect bounds;
        bounds.left = milton_state->picker.center.x - bound_radius_px;
        bounds.right = milton_state->picker.center.x + bound_radius_px;
        bounds.top = milton_state->picker.center.y - bound_radius_px;
        bounds.bottom = milton_state->picker.center.y + bound_radius_px;
        milton_state->picker.bounds = bounds;
        milton_state->picker.pixels = arena_alloc_array(
                milton_state->root_arena, (4 * bound_radius_px * bound_radius_px), uint32);
        picker_update(&milton_state->picker,
                (v2i){
                milton_state->picker.center.x + (int)(milton_state->picker.wheel_radius),
                milton_state->picker.center.y
                });
    }
    milton_state->brush_size = 10;

    Brush brush = { 0 };
    {
        brush.radius = milton_state->brush_size * milton_state->view_scale;
        brush.alpha = 0.5f;
        brush.color = hsv_to_rgb(milton_state->picker.hsv);
    }
    milton_state->brush = brush;

    int closest_power_of_two = (1 << 27);  // Ceiling of log2(width * height * bpp)
    milton_state->raster_buffer_size = closest_power_of_two;

    milton_state->raster_buffer = arena_alloc_array(milton_state->root_arena,
            milton_state->raster_buffer_size, uint8);

    milton_gl_backend_init(milton_state);
}

inline v2i canvas_to_raster(v2i screen_size, int32 view_scale, v2i canvas_point)
{
    v2i screen_center = invscale_v2i(screen_size, 2);
    v2i point = canvas_point;
    point = invscale_v2i(point, view_scale);
    point = add_v2i     ( point, screen_center );
    return point;
}

inline v2i raster_to_canvas(v2i screen_size, int32 view_scale, v2i raster_point)
{
    v2i screen_center = invscale_v2i(screen_size, 2);
    v2i canvas_point = raster_point;
    canvas_point = sub_v2i   ( canvas_point ,  screen_center );
    canvas_point = scale_v2i (canvas_point, view_scale);
    return canvas_point;
}

static Rect get_stroke_raster_bounds(
        v2i screen_size, int32 view_scale, Stroke* stroke, int32 start, Brush brush)
{
    v2i* points = stroke->points;
    int32 num_points = stroke->num_points;
    v2i point = canvas_to_raster(screen_size, view_scale, points[0]);

    Rect limits = { point.x, point.y, point.x, point.y };
    assert ( start < stroke->num_points );
    for (int i = start; i < num_points; ++i)
    {
        v2i point = canvas_to_raster(screen_size, view_scale, points[i]);
        if (point.x < limits.left)
            limits.left = point.x;
        if (point.x > limits.right)
            limits.right = point.x;
        if (point.y < limits.top)
            limits.top = point.y;
        if (point.y > limits.bottom)
            limits.bottom = point.y;
    }
    limits = rect_enlarge(limits, (brush.radius / view_scale));

    assert (limits.right >= limits.left);
    assert (limits.bottom >= limits.top);
    return limits;
}

static void stroke_clip_to_rect(Stroke* stroke, Rect rect)
{
    stroke->num_clipped_points = 0;
    if (stroke->num_points == 1)
    {
        if (is_inside_rect(rect, stroke->points[0]))
        {
            stroke->clipped_points[stroke->num_clipped_points++] = stroke->points[0];
        }
    }
    else
    {
        for (int32 point_i = 0; point_i < stroke->num_points - 1; ++point_i)
        {
            v2i a = stroke->points[point_i];
            v2i b = stroke->points[point_i + 1];

            // Very conservative...
            bool32 inside =
                !(
                        (a.x > rect.right && b.x > rect.right) ||
                        (a.x < rect.left && b.x < rect.left) ||
                        (a.y < rect.top && b.y < rect.top) ||
                        (a.y > rect.bottom && b.y > rect.bottom)
                 );

            // We can add the segment
            if (inside)
            {
                stroke->clipped_points[stroke->num_clipped_points++] = a;
                stroke->clipped_points[stroke->num_clipped_points++] = b;
            }
        }
    }
}


// This actually makes things faster
typedef struct LinkedList_Stroke_s
{
    Stroke* elem;
    struct LinkedList_Stroke_s* next;
} LinkedList_Stroke;

// Filter strokes and render them. See `render_strokes` for the one that should be called
static void render_strokes_in_rect(MiltonState* milton_state, Rect limits)
{
    uint32* pixels = (uint32*)milton_state->raster_buffer;
    Stroke* strokes = milton_state->strokes;

    Rect canvas_limits;
    canvas_limits.top_left = raster_to_canvas(milton_state->screen_size, milton_state->view_scale,
            limits.top_left);
    canvas_limits.bot_right = raster_to_canvas(milton_state->screen_size, milton_state->view_scale,
            limits.bot_right);

    LinkedList_Stroke* stroke_list = NULL;

    // Go backwards so that list is in the correct older->newer order.
    for (int stroke_i = milton_state->num_strokes; stroke_i >= 0; --stroke_i)
    {
        Stroke* stroke = NULL;
        if (stroke_i == milton_state->num_strokes)
        {
            stroke = &milton_state->working_stroke;
        }
        else
        {
            stroke = &strokes[stroke_i];
        }
        assert(stroke);
        Rect enlarged_limits = rect_enlarge(canvas_limits, stroke->brush.radius);
        stroke_clip_to_rect(stroke, enlarged_limits);
        if (stroke->num_clipped_points)
        {
            LinkedList_Stroke* list_elem = arena_alloc_elem(
                    milton_state->transient_arena, LinkedList_Stroke);

            LinkedList_Stroke* tail = stroke_list;
            list_elem->elem = stroke;
            list_elem->next = stroke_list;
            stroke_list = list_elem;
        }
        // TODO:
        // Check if `limits` lies completely inside stroke.
        // If so, don't add it to the list, just keep track of it so we can do
        // a cheap fill-pass.
        // TODO
        // Every stroke that fills and that is completely opaque resets every
        // stroke before it!
    }

    for (int j = limits.top; j < limits.bottom; ++j)
    {
        for (int i = limits.left; i < limits.right; ++i)
        {
            v2i raster_point = {i, j};
            v2i canvas_point = raster_to_canvas(
                    milton_state->screen_size, milton_state->view_scale, raster_point);

            // Clear color
            float dr = 1.0f;
            float dg = 1.0f;
            float db = 1.0f;
            float da = 1.0f;

            struct LinkedList_Stroke_s* list_iter = stroke_list;
            while(list_iter)
            {
                Stroke* stroke = list_iter->elem;

                assert (stroke);
                v2i* points = stroke->clipped_points;

                v2i min_point = {0};
                float min_dist = FLT_MAX;
                float dx = 0;
                float dy = 0;
                //int64 radius_squared = stroke->brush.radius * stroke->brush.radius;
                if (stroke->num_clipped_points == 1)
                {
                    min_point = points[0];
                    dx = (float) (canvas_point.x - min_point.x);
                    dy = (float) (canvas_point.y - min_point.y);
                    min_dist = dx * dx + dy * dy;
                }
                else
                {
                    for (int point_i = 0; point_i < stroke->num_clipped_points - 1; point_i += 2)
                    {
                        // Find closest point.
                        v2i a = points[point_i];
                        v2i b = points[point_i + 1];

                        v2f ab = {(float)b.x - a.x, (float)b.y - a.y};
                        float mag_ab2 = ab.x * ab.x + ab.y * ab.y;
                        if (mag_ab2 > 0)
                        {
                            float mag_ab = sqrtf(mag_ab2);
                            float d_x = ab.x / mag_ab;
                            float d_y = ab.y / mag_ab;
                            // TODO: Maybe store these and not do conversion in the hot loop?
                            float ax_x = (float)(canvas_point.x - a.x);
                            float ax_y = (float)(canvas_point.y - a.y);
                            float disc = d_x * ax_x + d_y * ax_y;
                            v2i point;
                            if (disc >= 0 && disc <= mag_ab)
                            {
                                point = (v2i)
                                {
                                    (int32)(a.x + disc * d_x), (int32)(a.y + disc * d_y),
                                };
                            }
                            else if (disc < 0)
                            {
                                point = a;
                            }
                            else
                            {
                                point = b;
                            }
                            float test_dx = (float) (canvas_point.x - point.x);
                            float test_dy = (float) (canvas_point.y - point.y);
                            float dist = test_dx * test_dx + test_dy * test_dy;
                            if (dist < min_dist)
                            {
                                min_dist = dist;
                                min_point = point;
                                dx = test_dx;
                                dy = test_dy;
                            }
                        }
                    }
                }


                if (min_dist < FLT_MAX)
                {
                    int samples = 0;
                    {
                        float u = 0.223607f * milton_state->view_scale;  // sin(arctan(1/2)) / 2
                        float v = 0.670820f * milton_state->view_scale;  // cos(arctan(1/2)) / 2 + u

                        float dists[4];
                        dists[0] = (dx - u) * (dx - u) + (dy - v) * (dy - v);
                        dists[1] = (dx - v) * (dx - v) + (dy + u) * (dy + u);
                        dists[2] = (dx + u) * (dx + u) + (dy + v) * (dy + v);
                        dists[3] = (dx + v) * (dx + v) + (dy + u) * (dy + u);
                        for (int i = 0; i < 4; ++i)
                        {
                            if (sqrtf(dists[i]) < stroke->brush.radius)
                            {
                                ++samples;
                            }
                        }
                    }

                    // If the stroke contributes to the pixel, do compositing.
                    if (samples > 0)
                    {
                        // Do compositing
                        // ---------------

                        float coverage = (float)samples / 4.0f;

                        float sr = stroke->brush.color.r;
                        float sg = stroke->brush.color.g;
                        float sb = stroke->brush.color.b;
                        float sa = stroke->brush.alpha;

                        sa *= coverage;

                        dr = (1 - sa) * dr + sa * sr;
                        dg = (1 - sa) * dg + sa * sg;
                        db = (1 - sa) * db + sa * sb;
                        da = sa + da * (1 - sa);
                    }
                }

                list_iter = list_iter->next;

            }
            // From [0, 1] to [0, 255]
            v4f d = {
                dr, dg, db, da
            };
            uint32 pixel = color_v4f_to_u32(milton_state->cm, d);
            pixels[j * milton_state->screen_size.w + i] = pixel;
        }
    }
}

static void render_strokes(MiltonState* milton_state, Rect limits)
{
    Rect* split_rects = NULL;
    int32 num_rects = rect_split(milton_state->transient_arena,
            limits, 20, 20, &split_rects);
    for (int i = 0; i < num_rects; ++i)
    {
        split_rects[i] = rect_clip_to_screen(split_rects[i], milton_state->screen_size);
        render_strokes_in_rect(milton_state, split_rects[i]);
    }
}

static void render_picker(ColorPicker* picker, ColorManagement cm,
        Rect draw_rect, uint32* pixels, v2i screen_size, int32 view_scale)
{
    v2f baseline = {1,0};

    v4f background_color =
    {
        0.5f,
        0.5f,
        0.55f,
        0.4f,
    };

    // Copy canvas buffer into picker buffer
    for (int j = draw_rect.top; j < draw_rect.bottom; ++j)
    {
        for (int i = draw_rect.left; i < draw_rect.right; ++i)
        {
            uint32 picker_i = (j - draw_rect.top) *( 2*picker->bound_radius_px ) + (i - draw_rect.left);
            uint32 src = pixels[j * screen_size.w + i];
            picker->pixels[picker_i] = src;
        }
    }

    // Render background color.
    for (int j = draw_rect.top; j < draw_rect.bottom; ++j)
    {
        for (int i = draw_rect.left; i < draw_rect.right; ++i)
        {
            uint32 picker_i = (j - draw_rect.top) *( 2*picker->bound_radius_px ) + (i - draw_rect.left);
            v4f dest = color_u32_to_v4f(cm, picker->pixels[picker_i]);
            float alpha = background_color.a;
            v4f result =
            {
                dest.r * (1 - alpha) + background_color.r * alpha,
                dest.g * (1 - alpha) + background_color.g * alpha,
                dest.b * (1 - alpha) + background_color.b * alpha,
                dest.a + (alpha * (1 - dest.a)),
            };
            picker->pixels[picker_i] = color_v4f_to_u32(cm, result);
        }
    }

    // render wheel
    for (int j = draw_rect.top; j < draw_rect.bottom; ++j)
    {
        for (int i = draw_rect.left; i < draw_rect.right; ++i)
        {
            uint32 picker_i = (j - draw_rect.top) *( 2*picker->bound_radius_px ) + (i - draw_rect.left);
            v2f point = {(float)i, (float)j};
            uint32 dest_color = picker->pixels[picker_i];

            int samples = 0;
            float angle = 0;
            {
                float u = 0.223607f;
                float v = 0.670820f;

                samples += (int)picker_hits_wheel(picker,
                        add_v2f(point, (v2f){-u, -v}));
                samples += (int)picker_hits_wheel(picker,
                        add_v2f(point, (v2f){-v, u}));
                samples += (int)picker_hits_wheel(picker,
                        add_v2f(point, (v2f){u, v}));
                samples += (int)picker_hits_wheel(picker,
                        add_v2f(point, (v2f){v, u}));
            }

            if (samples > 0)
            {
                float angle = picker_wheel_get_angle(picker, point);
                float degree = radians_to_degrees(angle);
                v3f hsv = { degree, 0.5f, 1.0f };
                v3f rgb = hsv_to_rgb(hsv);

                float contrib = samples / 4.0f;

                v4f d = color_u32_to_v4f(cm, dest_color);

                v4f result =
                {
                    ((1 - contrib) * (d.r)) + (contrib * (rgb.r)),
                    ((1 - contrib) * (d.g)) + (contrib * (rgb.g)),
                    ((1 - contrib) * (d.b)) + (contrib * (rgb.b)),
                    d.a + (contrib * (1 - d.a)),
                };
                uint32 color = color_v4f_to_u32(cm, result);
                picker->pixels[picker_i] = color;
            }
        }
    }
    for (int j = draw_rect.top; j < draw_rect.bottom; ++j)
    {
        for (int i = draw_rect.left; i < draw_rect.right; ++i)
        {
            v2f point = { (float)i, (float)j };
            uint32 picker_i = (j - draw_rect.top) *( 2*picker->bound_radius_px ) + (i - draw_rect.left);
            uint32 dest_color = picker->pixels[picker_i];
            // MSAA!!
            int samples = 0;
            {
                float u = 0.223607f;
                float v = 0.670820f;

                samples += (int)is_inside_triangle(add_v2f(point, (v2f){-u, -v}),
                        picker->a, picker->b, picker->c);
                samples += (int)is_inside_triangle(add_v2f(point, (v2f){-v, u}),
                        picker->a, picker->b, picker->c);
                samples += (int)is_inside_triangle(add_v2f(point, (v2f){u, v}),
                        picker->a, picker->b, picker->c);
                samples += (int)is_inside_triangle(add_v2f(point, (v2f){v, u}),
                        picker->a, picker->b, picker->c);
            }

            if (samples > 0)
            {
                v3f hsv = picker_hsv_from_point(picker, point);

                float contrib = samples / 4.0f;

                v4f d = color_u32_to_v4f(cm, dest_color);

                v3f rgb = hsv_to_rgb(hsv);

                v4f result =
                {
                    ((1 - contrib) * (d.r)) + (contrib * (rgb.r)),
                    ((1 - contrib) * (d.g)) + (contrib * (rgb.g)),
                    ((1 - contrib) * (d.b)) + (contrib * (rgb.b)),
                    d.a + (contrib * (1 - d.a)),
                };

                picker->pixels[picker_i] = color_v4f_to_u32(cm, result);
            }
        }
    }

    // Blit picker pixels
    uint32* to_blit = picker->pixels;
    for (int j = draw_rect.top; j < draw_rect.bottom; ++j)
    {
        for (int i = draw_rect.left; i < draw_rect.right; ++i)
        {
            pixels[j * screen_size.w + i] = *to_blit++;
        }
    }
}

inline bool32 is_user_drawing(MiltonState* milton_state)
{
    bool32 result = milton_state->working_stroke.num_points > 0;
    return result;
}

// Returns non-zero if the raster buffer was modified by this update.
static bool32 milton_update(MiltonState* milton_state, MiltonInput* input)
{
    arena_reset(milton_state->transient_arena);
    bool32 updated = false;
    bool32 selector_updated = false;

    if (input->scale)
    {
        input->full_refresh = true;
        static float scale_factor = 1.5f;
        static int32 view_scale_limit = 1000000;
        if (input->scale > 0 && milton_state->view_scale > 2)
        {
            milton_state->view_scale = (int32)(milton_state->view_scale / scale_factor);
        }
        else if (milton_state->view_scale < view_scale_limit)
        {
            milton_state->view_scale = (int32)(milton_state->view_scale * scale_factor) + 1;
        }
        milton_state->brush.radius = milton_state->brush_size * milton_state->view_scale;
    }

    if (input->reset)
    {
        milton_state->num_strokes = 0;
        milton_state->strokes[0].num_points = 0;
        milton_state->working_stroke.num_points = 0;
        input->full_refresh = true;
    }

    bool32 finish_stroke = false;
    if (input->point)
    {
        v2i point = *input->point;
        if (!is_user_drawing(milton_state) && is_inside_picker(&milton_state->picker, point))
        {
            ColorPickResult pick_result = picker_update(&milton_state->picker, point);
            if (pick_result & ColorPickResult_change_color)
            {
                milton_state->brush.color = hsv_to_rgb(milton_state->picker.hsv);
            }
            milton_state->canvas_blocked = true;
            selector_updated = true;
        }
        else if (!milton_state->canvas_blocked)
        {
            v2i in_point = *input->point;

            // Avoid creating really large update rects when starting. new strokes
            if (milton_state->working_stroke.num_points == 0)
            {
                milton_state->last_point = in_point;
            }

            v2i canvas_point = raster_to_canvas(milton_state->screen_size, milton_state->view_scale, in_point);

            // TODO: make deque!!
            if (milton_state->working_stroke.num_points < LIMIT_STROKE_POINTS)
            {
                // Add to current stroke.
                milton_state->working_stroke.points[milton_state->working_stroke.num_points++] = canvas_point;
                milton_state->working_stroke.brush = milton_state->brush;
                milton_state->working_stroke.bounds =
                    bounding_rect_for_points(milton_state->working_stroke.points,
                            milton_state->working_stroke.num_points);

            }

            milton_state->last_point = in_point;

            updated = true;
        }
        if (milton_state->canvas_blocked)
        {
            v2f fpoint = v2i_to_v2f(point);
            ColorPicker* picker = &milton_state->picker;
            if  (picker_wheel_active(picker))
            {
                //if (picker_is_within_wheel(picker, fpoint))
                if (is_inside_triangle(fpoint, picker->a, picker->b, picker->c))
                {
                    picker_wheel_deactivate(picker);
                }
                else
                {
                    picker_update_wheel(&milton_state->picker, fpoint);
                    milton_state->brush.color = hsv_to_rgb(milton_state->picker.hsv);
                }
                selector_updated = true;
            }
        }
    }
    if (input->end_stroke)
    {
        if (milton_state->canvas_blocked)
        {
            picker_wheel_deactivate(&milton_state->picker);
            milton_state->canvas_blocked = false;
        }
        else
        {
            if (milton_state->num_strokes < 4096)
            {
                // Copy current stroke.
                milton_state->strokes[milton_state->num_strokes++] = milton_state->working_stroke;
                // Clear working_stroke
                {
                    milton_state->working_stroke.num_points = 0;
                }
            }
        }
    }

    Rect limits = { 0 };

    if (input->full_refresh)
    {
        limits.left = 0;
        limits.right = milton_state->screen_size.w;
        limits.top = 0;
        limits.bottom = milton_state->screen_size.h;
    }
    else if (milton_state->working_stroke.num_points > 1)
    {
        Stroke* stroke = &milton_state->working_stroke;
        v2i new_point = canvas_to_raster(
                    milton_state->screen_size, milton_state->view_scale,
                    stroke->points[stroke->num_points - 2]);

        limits.left =   min (milton_state->last_point.x, new_point.x);
        limits.right =  max (milton_state->last_point.x, new_point.x);
        limits.top =    min (milton_state->last_point.y, new_point.y);
        limits.bottom = max (milton_state->last_point.y, new_point.y);
        limits = rect_enlarge(limits, (stroke->brush.radius / milton_state->view_scale));
        limits = rect_clip_to_screen(limits, milton_state->screen_size);

        /* render_strokes_in_rect(milton_state, limits); */
    }
    else if (milton_state->working_stroke.num_points == 1)
    {
        Stroke* stroke = &milton_state->working_stroke;
        v2i point = canvas_to_raster(milton_state->screen_size, milton_state->view_scale,
                stroke->points[0]);
        int32 raster_radius = stroke->brush.radius / milton_state->view_scale;
        limits.left = -raster_radius  + point.x;
        limits.right = raster_radius  + point.x;
        limits.top = -raster_radius   + point.y;
        limits.bottom = raster_radius + point.y;
        limits = rect_clip_to_screen(limits, milton_state->screen_size);
        /* render_strokes_in_rect(milton_state, limits); */
    }

    render_strokes(milton_state, limits);

    // Render UI
    {
        Rect* split_rects = NULL;
        bool32 redraw = false;
        Rect draw_rect = picker_get_draw_rect(&milton_state->picker);
        int32 num_rects = rect_split(milton_state->transient_arena,
                draw_rect, 20, 20, &split_rects);
        for (int i = 0; i < num_rects; ++i)
        {
            Rect clipped = rect_intersect(split_rects[i], draw_rect);
            if ((clipped.left != clipped.right) && clipped.top != clipped.bottom)
            {
                redraw = true;
                break;
            }
        }
        if (redraw || selector_updated)
        {
            render_strokes(milton_state, draw_rect);
            render_picker(&milton_state->picker, milton_state->cm,
                    draw_rect, (uint32*)milton_state->raster_buffer,
                    milton_state->screen_size, milton_state->view_scale);
        }
    }

    updated = true;

    return updated;
}
