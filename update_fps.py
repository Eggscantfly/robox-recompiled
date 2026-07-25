import sys

with open('sdk/gx_ogl.c', 'r') as f:
    content = f.read()

# Add scale to draw_crash_text
content = content.replace(
    'static void draw_crash_text(int x, int y, const char *str, float r, float g, float b, float *verts, int *num_verts) {',
    'static void draw_crash_text(int x, int y, const char *str, float r, float g, float b, float *verts, int *num_verts, float scale) {'
)
content = content.replace(
    'float y0 = (float)(cursor_y + font_metrics[c].yOffset * 2);',
    'float y0 = (float)(cursor_y + font_metrics[c].yOffset * scale);'
)
content = content.replace(
    'float x1 = x0 + (float)(font_metrics[c].width * 2);',
    'float x1 = x0 + (float)(font_metrics[c].width * scale);'
)
content = content.replace(
    'float y1 = y0 + (float)(font_metrics[c].height * 2);',
    'float y1 = y0 + (float)(font_metrics[c].height * scale);'
)
content = content.replace(
    'cursor_x += (font_metrics[c].width + 1) * 2;',
    'cursor_x += (font_metrics[c].width + 1) * scale;'
)

# Update crash screen calls
content = content.replace(
    'draw_crash_text(64, 64, title ? title : "Fatal error", 0.9f, 0.9f, 0.9f, verts, &num_verts);',
    'draw_crash_text(64, 64, title ? title : "Fatal error", 0.9f, 0.9f, 0.9f, verts, &num_verts, 2.0f);'
)
content = content.replace(
    'if (details) draw_crash_text(64, 128, details, 0.9f, 0.9f, 0.0f, verts, &num_verts);',
    'if (details) draw_crash_text(64, 128, details, 0.9f, 0.9f, 0.0f, verts, &num_verts, 2.0f);'
)
content = content.replace(
    'if (regs) draw_crash_text(64, 224, regs, 0.9f, 0.9f, 0.0f, verts, &num_verts);',
    'if (regs) draw_crash_text(64, 224, regs, 0.9f, 0.9f, 0.0f, verts, &num_verts, 2.0f);'
)
content = content.replace(
    'draw_crash_text(64, 520, "Press ENTER to save log and exit", 0.9f, 0.9f, 0.0f, verts, &num_verts);',
    'draw_crash_text(64, 520, "Press ENTER to save log and exit", 0.9f, 0.9f, 0.0f, verts, &num_verts, 2.0f);'
)

# Update FPS call and increase scale
content = content.replace(
    'draw_crash_text(1050, 680, fps_str, r, g, b, verts, &num_verts);',
    'draw_crash_text(920, 650, fps_str, r, g, b, verts, &num_verts, 4.0f);'
)

# Hook into gx_ogl_present_bink
func_bink_idx = content.find('static int gx_ogl_present_bink(void) {')
if func_bink_idx != -1:
    swap_idx = content.find('if (win) SDL_GL_SwapWindow(win);', func_bink_idx)
    if swap_idx != -1:
        content = content[:swap_idx] + 'if (g_show_fps) gx_ogl_render_fps();\n    ' + content[swap_idx:]

with open('sdk/gx_ogl.c', 'w') as f:
    f.write(content)
