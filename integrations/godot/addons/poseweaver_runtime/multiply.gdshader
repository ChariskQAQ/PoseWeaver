shader_type canvas_item;
render_mode blend_mul, unshaded;
void fragment() {
    // Transparent pixels must multiply by white, preserving the background.
    COLOR.rgb = mix(vec3(1.0), COLOR.rgb, COLOR.a);
    COLOR.a = 1.0;
}
