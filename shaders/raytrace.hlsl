// One invocation traces one camera ray. The low-resolution UAVs are sampled
// by the fullscreen compositor after the compute pass completes.

RWTexture2D<float4> outputImage : register(u0);
RWTexture2D<float4> materialImage : register(u1);
RWStructuredBuffer<float4> accumulation : register(u2);
RWStructuredBuffer<float4> materialAccumulation : register(u3);

struct GpuObject
{
    float4 posRadius;
    float4 color;
    float mass;
    float padding0;
    float padding1;
    float padding2;
    float4 velocity;
};

StructuredBuffer<GpuObject> objects : register(t0);

cbuffer RaytraceConstants : register(b0)
{
    float3 cameraPos;
    float cameraPadding0;
    float3 target;
    float cameraPadding1;
    float fovYRadians;
    float aspect;
    uint renderWidth;
    uint renderHeight;
    uint maxSteps;
    float dLambda;
    float escapeR;
    float horizonR;
    float diskR1;
    float diskR2;
    uint objectCount;
    uint sampleIndex;
    float jitterX;
    float jitterY;
    float padding2;
    float padding3;
};

struct RayState
{
    float3 q;
    float3 v;
    float energy;
};

struct Derivative
{
    float3 dq;
    float3 dv;
};

float safeSin(float value)
{
    float result = sin(value);
    if(abs(result) < 1.0e-5)
        return result < 0.0 ? -1.0e-5 : 1.0e-5;
    return result;
}

float3 rayCartesian(RayState ray)
{
    float radius = ray.q.x;
    float theta = ray.q.y;
    float phi = ray.q.z;
    return float3(
        radius * sin(theta) * cos(phi),
        radius * sin(theta) * sin(phi),
        radius * cos(theta));
}

bool finiteFloat(float value)
{
    return !isnan(value) && !isinf(value);
}

bool finiteVec3(float3 value)
{
    return finiteFloat(value.x) && finiteFloat(value.y) && finiteFloat(value.z);
}

bool interceptDisk(float3 oldPos, float3 newPos, out float radiusAtHit)
{
    bool crossed = oldPos.y * newPos.y < 0.0;
    if(!crossed) return false;

    float denominator = newPos.y - oldPos.y;
    if(abs(denominator) < 1.0e-8) return false;

    float crossingT = clamp(-oldPos.y / denominator, 0.0, 1.0);
    float3 crossingPoint = lerp(oldPos, newPos, crossingT);
    float diskRadius = length(float2(crossingPoint.x, crossingPoint.z));
    if(diskRadius < diskR1 || diskRadius > diskR2) return false;

    radiusAtHit = diskRadius;
    return true;
}

bool interceptObject(
    float3 oldPos,
    float3 newPos,
    out float4 objectColor,
    out float3 hitCenter,
    out float hitRadius,
    out float3 hitPoint)
{
    float3 segment = newPos - oldPos;
    float a = dot(segment, segment);
    if(a < 1.0e-12) return false;

    bool foundHit = false;
    float closestT = 2.0;
    for(uint objectIndex = 0u; objectIndex < objectCount; ++objectIndex)
    {
        float3 center = objects[objectIndex].posRadius.xyz;
        float radius = objects[objectIndex].posRadius.w;
        float3 offset = oldPos - center;
        float b = 2.0 * dot(offset, segment);
        float c = dot(offset, offset) - radius * radius;
        float discriminant = b * b - 4.0 * a * c;
        if(discriminant < 0.0) continue;

        float squareRoot = sqrt(discriminant);
        float inverseDenominator = 0.5 / a;
        float nearT = (-b - squareRoot) * inverseDenominator;
        float farT = (-b + squareRoot) * inverseDenominator;
        float candidateT = nearT;
        if(candidateT < 0.0 || candidateT > 1.0) candidateT = farT;
        if(candidateT < 0.0 || candidateT > 1.0 || candidateT >= closestT)
            continue;

        closestT = candidateT;
        objectColor = objects[objectIndex].color;
        hitCenter = center;
        hitRadius = radius;
        hitPoint = oldPos + candidateT * segment;
        foundHit = true;
    }
    return foundHit;
}

RayState makeRay(float3 position, float3 direction)
{
    RayState ray;
    float radius = length(position);
    float phi = atan2(position.y, position.x);
    float theta = acos(clamp(position.z / radius, -1.0, 1.0));
    float sinTheta = safeSin(theta);
    float cosTheta = cos(theta);
    float sinPhi = sin(phi);
    float cosPhi = cos(phi);

    float dr = dot(position, direction) / radius;
    float dtheta =
        (direction.x * cosTheta * cosPhi +
         direction.y * cosTheta * sinPhi -
         direction.z * sinTheta) / radius;
    float dphi =
        (-direction.x * sinPhi + direction.y * cosPhi) /
        (radius * sinTheta);
    float f = max(1.0 - horizonR / radius, 1.0e-5);
    float angular = dtheta * dtheta + sinTheta * sinTheta * dphi * dphi;
    float energySquared = dr * dr + f * radius * radius * angular;

    ray.q = float3(radius, theta, phi);
    ray.v = float3(dr, dtheta, dphi);
    ray.energy = sqrt(max(energySquared, 0.0));
    return ray;
}

Derivative geodesicRHS(RayState ray)
{
    Derivative result;
    float radius = max(ray.q.x, horizonR * 1.00001);
    float theta = ray.q.y;
    float dr = ray.v.x;
    float dtheta = ray.v.y;
    float dphi = ray.v.z;
    float sinTheta = safeSin(theta);
    float cosTheta = cos(theta);
    float f = max(1.0 - horizonR / radius, 1.0e-5);
    float dtDlambda = ray.energy / f;

    result.dq = ray.v;
    float d2r =
        -(horizonR / (2.0 * radius * radius)) * f *
            dtDlambda * dtDlambda +
        (horizonR / (2.0 * radius * radius * f)) * dr * dr +
        radius * f *
            (dtheta * dtheta + sinTheta * sinTheta * dphi * dphi);
    float d2theta =
        -(2.0 / radius) * dr * dtheta +
        sinTheta * cosTheta * dphi * dphi;
    float d2phi =
        -(2.0 / radius) * dr * dphi -
        2.0 * cosTheta / sinTheta * dtheta * dphi;
    result.dv = float3(d2r, d2theta, d2phi);
    return result;
}

RayState addState(RayState base, Derivative derivative, float factor)
{
    RayState result = base;
    result.q = base.q + factor * derivative.dq;
    result.v = base.v + factor * derivative.dv;
    return result;
}

void rk4Step(inout RayState ray)
{
    Derivative k1 = geodesicRHS(ray);
    Derivative k2 = geodesicRHS(addState(ray, k1, dLambda * 0.5));
    Derivative k3 = geodesicRHS(addState(ray, k2, dLambda * 0.5));
    Derivative k4 = geodesicRHS(addState(ray, k3, dLambda));
    ray.q += (dLambda / 6.0) *
        (k1.dq + 2.0 * k2.dq + 2.0 * k3.dq + k4.dq);
    ray.v += (dLambda / 6.0) *
        (k1.dv + 2.0 * k2.dv + 2.0 * k3.dv + k4.dv);
}

[numthreads(8, 8, 1)]
void CSMain(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    uint2 gid = dispatchThreadId.xy;
    if(gid.x >= renderWidth || gid.y >= renderHeight) return;

    float3 forward = normalize(target - cameraPos);
    float3 worldUp = float3(0.0, 1.0, 0.0);
    float3 right = cross(forward, worldUp);
    if(dot(right, right) < 1.0e-8)
        right = float3(0.0, 0.0, 1.0);
    else
        right = normalize(right);
    float3 up = normalize(cross(right, forward));

    float tanHalfFov = tan(fovYRadians * 0.5);
    float sampleX = (float)gid.x + 0.5 + jitterX;
    float sampleY = (float)gid.y + 0.5 + jitterY;
    float u =
        (2.0 * (sampleX / (float)renderWidth) - 1.0) *
        aspect * tanHalfFov;
    float v =
        (1.0 - 2.0 * (sampleY / (float)renderHeight)) * tanHalfFov;
    float3 direction = normalize(u * right + v * up + forward);

    RayState ray = makeRay(cameraPos, direction);
    bool captured = false;
    bool diskHit = false;
    bool objectHit = false;
    float diskRadiusAtHit = 0.0;
    float4 objectColor = 0.0.xxxx;
    float3 hitCenter = 0.0.xxx;
    float hitRadius = 0.0;
    float3 objectHitPoint = 0.0.xxx;

    for(uint step = 0u; step < maxSteps; ++step)
    {
        if(ray.q.x <= horizonR * 1.01)
        {
            captured = true;
            break;
        }
        if(ray.q.x > escapeR && ray.v.x > 0.0) break;

        float previousRadius = ray.q.x;
        float3 oldPos = rayCartesian(ray);
        rk4Step(ray);
        if(!finiteVec3(ray.q) || !finiteVec3(ray.v))
        {
            if(previousRadius <= horizonR * 1.02) captured = true;
            break;
        }

        float3 newPos = rayCartesian(ray);
        if(interceptObject(
               oldPos,
               newPos,
               objectColor,
               hitCenter,
               hitRadius,
               objectHitPoint))
        {
            objectHit = true;
            break;
        }
        if(interceptDisk(oldPos, newPos, diskRadiusAtHit))
        {
            diskHit = true;
            break;
        }
        if(ray.q.x <= horizonR * 1.01)
        {
            captured = true;
            break;
        }
    }

    uint index = gid.y * renderWidth + gid.x;
    float4 currentColor = 0.0.xxxx;
    float4 currentMaterial = 0.0.xxxx;
    if(objectHit)
    {
        float3 normal = normalize(
            (objectHitPoint - hitCenter) / max(hitRadius, 1.0e-6));
        float3 lightDirection = normalize(float3(-0.35, 0.80, 0.48));
        float3 viewDirection = normalize(cameraPos - objectHitPoint);
        float diffuse = max(dot(normal, lightDirection), 0.0);
        float3 reflectedLight = reflect(-lightDirection, normal);
        float specular =
            pow(max(dot(reflectedLight, viewDirection), 0.0), 24.0) * 0.30;
        float3 shadedColor = clamp(
            objectColor.rgb * (0.22 + 0.78 * diffuse) + specular,
            0.0,
            1.0);
        float objectAlpha = clamp(objectColor.a, 0.0, 1.0);
        currentColor = float4(shadedColor * objectAlpha, objectAlpha);
        currentMaterial.b = 1.0;
    }
    else if(diskHit)
    {
        float diskT = clamp(
            (diskRadiusAtHit - diskR1) / (diskR2 - diskR1),
            0.0,
            1.0);
        float3 innerColor = float3(1.0, 0.15, 0.01);
        float3 outerColor = float3(1.0, 0.72, 0.10);
        float3 diskColor = lerp(innerColor, outerColor, diskT);
        currentColor = float4(diskColor, 1.0);
        currentMaterial.g = 1.0;
    }
    else if(captured)
    {
        currentColor = float4(0.0, 0.0, 0.0, 1.0);
        currentMaterial.r = 1.0;
    }

    float4 accumulatedColor = currentColor;
    float4 accumulatedMaterial = currentMaterial;
    if(sampleIndex > 0u)
    {
        float sampleWeight = 1.0 / (float)(sampleIndex + 1u);
        accumulatedColor = lerp(
            accumulation[index],
            currentColor,
            sampleWeight);
        accumulatedMaterial = lerp(
            materialAccumulation[index],
            currentMaterial,
            sampleWeight);
    }

    accumulatedColor = clamp(accumulatedColor, 0.0, 1.0);
    accumulatedMaterial = clamp(accumulatedMaterial, 0.0, 1.0);
    accumulation[index] = accumulatedColor;
    materialAccumulation[index] = accumulatedMaterial;
    outputImage[gid] = accumulatedColor;
    materialImage[gid] = accumulatedMaterial;
}
