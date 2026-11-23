// HUD-less (design §4): the frame NR leaves the HUD alone in. out = F + (1 - A) * (N - H), in stored values: F the final frame,
// N the HUD-less image after NR, H the game's HUD-less image, A the HUD's opacity. Exact where A is 0 (the scene takes NR's
// change) and 1 (the HUD is the game's). Drawn over the back buffer by decode_vs's fullscreen triangle.
Texture2D<float4> final_frame : register(t0);
Texture2D<float4> nr_hudless : register(t1);
Texture2D<float4> game_hudless : register(t2);
Texture2D<float4> opacity : register(t3);

cbuffer HudCompose : register(b0) {
  uint alpha_channel;  // 0: the opacity is in .r (a single-channel UI alpha); 3: in .a (UI colour and alpha, A8, four channels)
  uint clamp_unit;     // 1 for UNORM back buffers; 0 for float ones, which keep values beyond [0, 1]
};

float4 main(float4 position : SV_Position) : SV_Target {
  const int3 texel = int3(int2(position.xy), 0);
  const float4 final_value = final_frame.Load(texel);
  const float4 alpha_texel = opacity.Load(texel);
  const float alpha = (alpha_channel == 3u ? alpha_texel.a : alpha_texel.r);
  float4 result = final_value;
  result.rgb = final_value.rgb + (1.f - alpha) * (nr_hudless.Load(texel).rgb - game_hudless.Load(texel).rgb);
  return (clamp_unit != 0u ? saturate(result) : result);
}
