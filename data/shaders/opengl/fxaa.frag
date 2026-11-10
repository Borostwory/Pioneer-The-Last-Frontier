// Copyright © 2008-2026 Pioneer Developers. See AUTHORS.txt for details
// Licensed under the terms of the GPL v3. See licenses/GPL-3.txt

// Edge smoothing, the simple way (ScreenEffects.cpp): FXAA 3.11's quality pass (Timothy Lottes, NVIDIA, 2011), its
// preset 29 (12 steps along an edge), written out for this game. On the finished picture, whose colours are already
// as the screen shows them: from the brightness of each pixel and its neighbours it finds an edge, follows it both
// ways to its ends, and blends the pixel with its neighbour across the edge by how far it is along it.

#include "attributes.glsl"

uniform sampler2D colourTex;
// xy: 1 / the screen's size (px)
uniform vec4 EdgeScreen;

in vec2 v_uv;
out vec4 frag_color;

// how sharp an edge must be to smooth it: a share of the brightest pixel's brightness, and at least this much
#define FXAA_EDGE_THRESHOLD 0.125
#define FXAA_EDGE_THRESHOLD_MIN 0.0625
// how much of the blending within a pixel (for edges thinner than a pixel): 0 none, 1 the most
#define FXAA_SUBPIX 0.75
#define FXAA_STEPS 12

const float FXAA_STEP[FXAA_STEPS] = float[FXAA_STEPS](1.0, 1.5, 2.0, 2.0, 2.0, 2.0, 2.0, 2.0, 2.0, 2.0, 4.0, 8.0);

float fxaaLuma(vec4 c)
{
	return dot(c.rgb, vec3(0.299, 0.587, 0.114));
}

float fxaaLumaAt(vec2 uv)
{
	return fxaaLuma(textureLod(colourTex, uv, 0.0));
}

void main(void)
{
	vec2 rcp = EdgeScreen.xy;
	vec2 posM = v_uv;
	vec4 rgbM = textureLod(colourTex, posM, 0.0);
	float lumaM = fxaaLuma(rgbM);
	float lumaS = fxaaLuma(textureLodOffset(colourTex, posM, 0.0, ivec2(0, 1)));
	float lumaE = fxaaLuma(textureLodOffset(colourTex, posM, 0.0, ivec2(1, 0)));
	float lumaN = fxaaLuma(textureLodOffset(colourTex, posM, 0.0, ivec2(0, -1)));
	float lumaW = fxaaLuma(textureLodOffset(colourTex, posM, 0.0, ivec2(-1, 0)));

	float rangeMax = max(max(lumaN, lumaW), max(lumaE, max(lumaS, lumaM)));
	float rangeMin = min(min(lumaN, lumaW), min(lumaE, min(lumaS, lumaM)));
	float range = rangeMax - rangeMin;
	if (range < max(FXAA_EDGE_THRESHOLD_MIN, rangeMax * FXAA_EDGE_THRESHOLD)) {
		frag_color = rgbM;
		return;
	}

	float lumaNW = fxaaLuma(textureLodOffset(colourTex, posM, 0.0, ivec2(-1, -1)));
	float lumaSE = fxaaLuma(textureLodOffset(colourTex, posM, 0.0, ivec2(1, 1)));
	float lumaNE = fxaaLuma(textureLodOffset(colourTex, posM, 0.0, ivec2(1, -1)));
	float lumaSW = fxaaLuma(textureLodOffset(colourTex, posM, 0.0, ivec2(-1, 1)));

	// which way the edge runs: along the rows or the columns
	float lumaNS = lumaN + lumaS;
	float lumaWE = lumaW + lumaE;
	float subpixRcpRange = 1.0 / range;
	float subpixNSWE = lumaNS + lumaWE;
	float edgeHorz1 = -2.0 * lumaM + lumaNS;
	float edgeVert1 = -2.0 * lumaM + lumaWE;
	float lumaNESE = lumaNE + lumaSE;
	float lumaNWNE = lumaNW + lumaNE;
	float edgeHorz2 = -2.0 * lumaE + lumaNESE;
	float edgeVert2 = -2.0 * lumaN + lumaNWNE;
	float lumaNWSW = lumaNW + lumaSW;
	float lumaSWSE = lumaSW + lumaSE;
	float edgeHorz4 = abs(edgeHorz1) * 2.0 + abs(edgeHorz2);
	float edgeVert4 = abs(edgeVert1) * 2.0 + abs(edgeVert2);
	float edgeHorz3 = -2.0 * lumaW + lumaNWSW;
	float edgeVert3 = -2.0 * lumaS + lumaSWSE;
	float edgeHorz = abs(edgeHorz3) + edgeHorz4;
	float edgeVert = abs(edgeVert3) + edgeVert4;

	float subpixNWSWNESE = lumaNWSW + lumaNESE;
	float lengthSign = rcp.x;
	bool horzSpan = edgeHorz >= edgeVert;
	float subpixA = subpixNSWE * 2.0 + subpixNWSWNESE;
	if (!horzSpan) {
		lumaN = lumaW;
		lumaS = lumaE;
	} else {
		lengthSign = rcp.y;
	}
	float subpixB = subpixA * (1.0 / 12.0) - lumaM;

	// which side of the pixel the edge is on
	float gradientN = lumaN - lumaM;
	float gradientS = lumaS - lumaM;
	float lumaNN = lumaN + lumaM;
	float lumaSS = lumaS + lumaM;
	bool pairN = abs(gradientN) >= abs(gradientS);
	float gradient = max(abs(gradientN), abs(gradientS));
	if (pairN)
		lengthSign = -lengthSign;
	float subpixC = clamp(abs(subpixB) * subpixRcpRange, 0.0, 1.0);

	// follow the edge both ways until the brightness along it changes
	vec2 posB = posM;
	vec2 offNP = vec2(horzSpan ? rcp.x : 0.0, horzSpan ? 0.0 : rcp.y);
	if (!horzSpan)
		posB.x += lengthSign * 0.5;
	else
		posB.y += lengthSign * 0.5;
	vec2 posN = posB - offNP * FXAA_STEP[0];
	vec2 posP = posB + offNP * FXAA_STEP[0];
	float subpixD = -2.0 * subpixC + 3.0;
	float lumaEndN = fxaaLumaAt(posN);
	float subpixE = subpixC * subpixC;
	float lumaEndP = fxaaLumaAt(posP);
	if (!pairN)
		lumaNN = lumaSS;
	float gradientScaled = gradient * 0.25;
	float lumaMM = lumaM - lumaNN * 0.5;
	float subpixF = subpixD * subpixE;
	bool lumaMLTZero = lumaMM < 0.0;
	lumaEndN -= lumaNN * 0.5;
	lumaEndP -= lumaNN * 0.5;
	bool doneN = abs(lumaEndN) >= gradientScaled;
	bool doneP = abs(lumaEndP) >= gradientScaled;
	if (!doneN)
		posN -= offNP * FXAA_STEP[1];
	if (!doneP)
		posP += offNP * FXAA_STEP[1];
	for (int i = 2; i < FXAA_STEPS && !(doneN && doneP); i++) {
		if (!doneN)
			lumaEndN = fxaaLumaAt(posN) - lumaNN * 0.5;
		if (!doneP)
			lumaEndP = fxaaLumaAt(posP) - lumaNN * 0.5;
		doneN = doneN || abs(lumaEndN) >= gradientScaled;
		doneP = doneP || abs(lumaEndP) >= gradientScaled;
		if (!doneN)
			posN -= offNP * FXAA_STEP[i];
		if (!doneP)
			posP += offNP * FXAA_STEP[i];
	}

	// how far along the edge this pixel is, and so how far to move its read across it
	float dstN = horzSpan ? posM.x - posN.x : posM.y - posN.y;
	float dstP = horzSpan ? posP.x - posM.x : posP.y - posM.y;
	bool goodSpanN = (lumaEndN < 0.0) != lumaMLTZero;
	bool goodSpanP = (lumaEndP < 0.0) != lumaMLTZero;
	float spanLengthRcp = 1.0 / (dstP + dstN);
	bool directionN = dstN < dstP;
	float dst = min(dstN, dstP);
	bool goodSpan = directionN ? goodSpanN : goodSpanP;
	float subpixG = subpixF * subpixF;
	float pixelOffset = dst * -spanLengthRcp + 0.5;
	float subpixH = subpixG * FXAA_SUBPIX;
	float pixelOffsetGood = goodSpan ? pixelOffset : 0.0;
	float pixelOffsetSubpix = max(pixelOffsetGood, subpixH);
	if (!horzSpan)
		posM.x += pixelOffsetSubpix * lengthSign;
	else
		posM.y += pixelOffsetSubpix * lengthSign;
	frag_color = vec4(textureLod(colourTex, posM, 0.0).rgb, rgbM.a);
}
