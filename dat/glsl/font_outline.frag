uniform vec4 color;
uniform vec4 outline_color;
uniform sampler2D sampler;

in vec2 tex_coord_out;
out vec4 color_out;

void main(void) {
   // dist is a value between 0 and 1 with 0.5 on the edge and 1 inside it.
   // "alpha" indicates if we're in the glyph (0 for no, 1 for yes).
   // "beta" indicates if we're in the glyph or outline (0 for no, 1 for yes).
   float dist = texture(sampler, tex_coord_out).r;
   float adx = abs(dFdx(dist));
   float ady = abs(dFdy(dist));
   color_out = vec4(dist, adx, ady, step(1e-4, dist) );

#include "colorblind.glsl"
}
