
//
// This source file is part of appleseed.
// Visit http://appleseedhq.net/ for additional information and resources.
//
// This software is released under the MIT license.
//
// Copyright (c) 2017 Artem Bishev, The appleseedhq Organization
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
// THE SOFTWARE.
//

// Interface header.
#include "randomwalkbssrdf.h"

// appleseed.renderer headers.
#include "renderer/kernel/intersection/intersector.h"
#include "renderer/kernel/shading/shadingcontext.h"
#include "renderer/kernel/shading/shadingpoint.h"
#include "renderer/modeling/bsdf/bsdf.h"
#include "renderer/modeling/bsdf/bsdfsample.h"
#include "renderer/modeling/bssrdf/sss.h"
#include "renderer/modeling/bssrdf/bssrdfsample.h"

// appleseed.foundation headers.
#include "foundation/math/phasefunction.h"
#include "foundation/math/sampling/mappings.h"
#include "foundation/math/rr.h"
#include "foundation/math/scalar.h"
#include "foundation/math/vector.h"
#include "foundation/utility/api/specializedapiarrays.h"
#include "foundation/utility/containers/dictionary.h"

#include "foundation/math/cdf.h"
// Standard headers.
#include <algorithm>
#include <cstddef>

// Forward declarations.
namespace foundation    { class Arena; }
namespace renderer      { class BSDFSample; }
namespace renderer      { class BSSRDFSample; }
namespace renderer      { class ShadingContext; }

using namespace foundation;
using namespace std;

namespace renderer
{

namespace
{
    //
    // Random-walk BSSRDF.
    //
    // Reference:
    //
    //   Path Traced Subsurface Scattering using Anisotropic Phase Functions
    //   and Non-Exponential Free Flights, Pixar Technical Memo 17-07
    //   https://graphics.pixar.com/library/PathTracedSubsurface/paper.pdf
    //

    const char* Model = "randomwalk_bssrdf";

    class RandomWalkBSSRDF
      : public BSSRDF
    {
      public:
        RandomWalkBSSRDF(
            const char*             name,
            const ParamArray&       params)
          : BSSRDF(name, params)
        {
            m_inputs.declare("weight", InputFormatFloat, "1.0");
            m_inputs.declare("reflectance", InputFormatSpectralReflectance);
            m_inputs.declare("reflectance_multiplier", InputFormatFloat, "1.0");
            m_inputs.declare("mfp", InputFormatSpectralReflectance);
            m_inputs.declare("mfp_multiplier", InputFormatFloat, "1.0");
            m_inputs.declare("ior", InputFormatFloat);
            m_inputs.declare("fresnel_weight", InputFormatFloat, "1.0");
            m_inputs.declare("zero_scattering_weight", InputFormatFloat, "1.0");
            m_inputs.declare("single_scattering_weight", InputFormatFloat, "1.0");
            m_inputs.declare("multiple_scattering_weight", InputFormatFloat, "1.0");

            const string volume_name = string(name) + "_volume";

            const string brdf_name = string(name) + "_brdf";
            m_brdf = LambertianBRDFFactory().create(brdf_name.c_str(), ParamArray()).release();
            m_brdf_data.m_reflectance.set(1.0f);
            m_brdf_data.m_reflectance_multiplier = 1.0f;
        }

        void release() override
        {
            delete this;
        }

        const char* get_model() const override
        {
            return Model;
        }

        size_t compute_input_data_size() const override
        {
            return sizeof(RandomWalkBSSRDFInputValues);
        }

        void prepare_inputs(
            Arena&                  arena,
            const ShadingPoint&     shading_point,
            void*                   data) const override
        {
            RandomWalkBSSRDFInputValues* values =
                static_cast<RandomWalkBSSRDFInputValues*>(data);

            auto& precomputed = values->m_precomputed;

            // Apply multipliers to input values.
            values->m_reflectance *= values->m_reflectance_multiplier;
            values->m_mfp *= values->m_mfp_multiplier;

            // Clamp input values.
            clamp_in_place(values->m_reflectance, 0.001f, 0.999f);
            clamp_low_in_place(values->m_mfp, 1.0e-6f);

            for (size_t i = 0, e = Spectrum::size(); i < e; ++i)
            {
                // Compute single-scattering albedo from multiple-scattering albedo.
                const float albedo = albedo_from_reflectance(values->m_reflectance[i]);

                // Compute extinction coefficient.
                const float s = normalized_diffusion_s_mfp(values->m_reflectance[i]);
                const float extinction = rcp(values->m_mfp[i] * s);

                precomputed.m_albedo[i] = albedo;
                precomputed.m_extinction[i] = extinction;
                precomputed.m_scattering[i] = albedo * extinction;

                // Compute diffusion length, required by Dwivedi sampling.
                const float kappa = min(compute_rcp_diffusion_length(albedo), 0.99f);
                precomputed.m_rcp_diffusion_length[i] = kappa;
            }
        }

        static float compute_rcp_diffusion_length_low_albedo(const float albedo)
        {
            const float a = rcp(albedo);
            const float b = exp(-2.0f * a);

            const float x[4] = {
                1.0f,
                a * 4.0f - 1.0f,
                a * (a * 24.0f - 12.0f) + 1.0f,
                a * (a * (a * 512.0f - 384.0f) + 72.0f) - 3.0f };

            return 1.0f - 2.0f * b * (x[0] + b * (x[1] + b * (x[2] + b * x[3])));
        }

        static float compute_rcp_diffusion_length_high_albedo(const float albedo)
        {
            const float a = 1.0f - albedo;
            const float b = sqrt(3.0f * a);

            const float x[5] = {
                +1.0000000000f,
                -0.4000000000f,
                -0.0685714286f,
                -0.0160000000f,
                -0.0024638218f };

            return b * (x[0] + a * (x[1] + a * (x[2] + a * (x[3] + a * x[4]))));
        }

        static float compute_rcp_diffusion_length(const float albedo)
        {
            const float a = clamp(albedo, 0.0f, 0.999f);
            return a < 0.56f ?
                compute_rcp_diffusion_length_low_albedo(a) :
                compute_rcp_diffusion_length_high_albedo(a);
        }

        static float albedo_from_reflectance(const float r)
        {
            return 1.0f - exp(r * (-5.09406f + r * (2.61188f - 4.31805f * r)));
        }

        static bool test_rr(
            SamplingContext&    sampling_context,
            BSSRDFSample&       bssrdf_sample)
        {
            // Generate a uniform sample in [0,1).
            sampling_context.split_in_place(1, 1);
            const float s = sampling_context.next2<float>();

            // Compute the probability of extending this path.
            const float scattering_prob =
                std::min(max_value(bssrdf_sample.m_value), 0.99f);

            // Russian Roulette.
            if (!pass_rr(scattering_prob, s))
                return false;

            // Adjust throughput to account for terminated paths.
            assert(scattering_prob > 0.0f);
            bssrdf_sample.m_value /= scattering_prob;

            return true;
        }

        static float compute_classical_sampling_probability(const float anisotropy)
        {
            return max(0.1f, pow(abs(anisotropy), 3.0f));
        }

        bool sample(
            const ShadingContext&   shading_context,
            SamplingContext&        sampling_context,
            const void*             data,
            const ShadingPoint&     outgoing_point,
            const Vector3f&         outgoing_dir,
            BSSRDFSample&           bssrdf_sample,
            BSDFSample&             bsdf_sample) const override
        {
            // Get input values.
            const RandomWalkBSSRDFInputValues* values =
                static_cast<const RandomWalkBSSRDFInputValues*>(data);
            const Spectrum& extinction = values->m_precomputed.m_extinction;
            const Spectrum& albedo = values->m_precomputed.m_albedo;
            const Spectrum& scattering = values->m_precomputed.m_scattering;
            const Spectrum& rcp_diffusion_length = values->m_precomputed.m_rcp_diffusion_length;

            // Compute the probability of classical sampling.
            // The probability of classical sampling is high if phase function is anisotropic.
            const float classical_sampling_prob = 0.1f;

            // Initialize BSSRDF value.
            bssrdf_sample.m_value.set(1.0f);
            bssrdf_sample.m_probability = 1.0f;

            // Pick initial random-walk direction uniformly.
            sampling_context.split_in_place(2, 1);
            Vector3d initial_dir = sample_hemisphere_cosine(sampling_context.next2<Vector2d>());
            initial_dir.y = -initial_dir.y;
            initial_dir = outgoing_point.get_shading_basis().transform_to_parent(initial_dir);
            ShadingRay ray = ShadingRay(
                outgoing_point.get_point(),
                initial_dir,
                outgoing_point.get_time(),
                VisibilityFlags::ShadowRay,
                outgoing_point.get_ray().m_depth + 1);

            // Choose color channel used for distance sampling.
            sampling_context.split_in_place(1, 1);
            Spectrum channel_cdf, channel_pdf;
            build_cdf_and_pdf(bssrdf_sample.m_value, channel_cdf, channel_pdf);
            const size_t channel = sample_cdf(
                &channel_cdf[0],
                &channel_cdf[0] + channel_cdf.size(),
                sampling_context.next2<float>());

            // Sample distance (we always use classical sampling here).
            sampling_context.split_in_place(1, 1);
            const float distance = sample_exponential_distribution(
                sampling_context.next2<float>(), extinction[channel]);

            // Trace the initial ray till we reach the surface from inside.
            bssrdf_sample.m_incoming_point.clear();
            shading_context.get_intersector().trace(
                ray,
                bssrdf_sample.m_incoming_point,
                &outgoing_point);
            if (!bssrdf_sample.m_incoming_point.is_valid())
                return false;
            const float ray_length = static_cast<float>(bssrdf_sample.m_incoming_point.get_distance());
            bool transmitted = ray_length <= distance;
            const float mis_weight = compute_transmission_classical_sampling(
                transmitted ? ray_length : distance,
                extinction,
                channel_pdf,
                transmitted,
                bssrdf_sample.m_value);
            bssrdf_sample.m_value *= rcp(mis_weight);

            // Do random walk until we reach the surface from inside.
            int n_iteration = 0;
            const int MaxIterationsCount = 64;
            const int MinRRIteration = 8;

            // Determine the closest surface point and corresponding slab normal.
            const float bias_strategy_prob =
                std::exp(-extinction[channel] * ray_length * 0.5f);
            const bool outgoing_point_is_closer = distance < 0.5f * ray_length;
            const Vector3f near_slab_normal = Vector3f(outgoing_point.get_geometric_normal());
            const Vector3f far_slab_normal = Vector3f(-bssrdf_sample.m_incoming_point.get_geometric_normal());
            const Vector3f& slab_normal = outgoing_point_is_closer ? far_slab_normal : near_slab_normal;
            Vector3d current_point = ray.point_at(distance);

            while (!transmitted)
            {
                if (++n_iteration > MaxIterationsCount)
                    return false;   // path got lost inside the object

                if (n_iteration > MinRRIteration && !test_rr(sampling_context, bssrdf_sample))
                    return false;   // sample has not passed Rusian Roulette test

                sampling_context.split_in_place(1, 2);

                // Choose color channel used for distance sampling.
                Spectrum channel_cdf, channel_pdf;
                build_cdf_and_pdf(bssrdf_sample.m_value, channel_cdf, channel_pdf);
                const size_t channel = sample_cdf(
                    &channel_cdf[0],
                    &channel_cdf[0] + channel_cdf.size(),
                    sampling_context.next2<float>());
                if (scattering[channel] == 0.0f || bssrdf_sample.m_value[channel] == 0.0f)
                    return false; // path got lost inside the object

                // Determine if we do Dwivedi (biased) sampling or classical sampling.
                bool is_biased = classical_sampling_prob < sampling_context.next2<float>();

                // Find next random-walk direction.
                sampling_context.split_in_place(2, 1);
                const Vector2f s_direction = sampling_context.next2<Vector2f>();
                Vector3f incoming;
                if (is_biased)
                {
                    DwivediPhaseFunction phase_function(rcp(rcp_diffusion_length[channel]));
                    phase_function.sample(slab_normal, s_direction, incoming);
                }
                else
                {
                    incoming = sample_sphere_uniform(s_direction);
                }

                // Update transmission by albedo.
                bssrdf_sample.m_value *= albedo;

                // Construct ray in the sampled direction.
                ray = ShadingRay(
                    current_point,
                    Vector3d(incoming),
                    outgoing_point.get_time(),
                    VisibilityFlags::ShadowRay,
                    ray.m_depth + 1
                );

                // Sample distance, assuming that the ray is infinite.
                sampling_context.split_in_place(1, 1);
                float cosine = dot(incoming, slab_normal);
                const float s_distance = sampling_context.next2<float>();
                const float effective_extinction = is_biased ?
                    extinction[channel] * (1.0f - cosine * rcp_diffusion_length[channel]) :
                    extinction[channel];
                float distance = sample_exponential_distribution(s_distance, effective_extinction);
                assert(distance > 0.0f && !isnan(distance) && !isinf(distance));

                // Trace ray up to the sampled distance.
                ray.m_tmax = distance;
                bssrdf_sample.m_incoming_point.clear();
                shading_context.get_intersector().trace(
                    ray,
                    bssrdf_sample.m_incoming_point);
                transmitted = bssrdf_sample.m_incoming_point.hit_surface();
                current_point = ray.point_at(distance);
                if (transmitted)
                {
                    distance = static_cast<float>(
                        bssrdf_sample.m_incoming_point.get_distance());
                }

                // Compute transmission for the distance sample.
                Spectrum transmission;
                const float mis_classical = compute_transmission_classical_sampling(
                    distance,
                    extinction,
                    channel_pdf,
                    transmitted,
                    transmission);
                const float mis_dwivedi_near = compute_mis_dwivedi_sampling(
                    distance,
                    extinction,
                    rcp_diffusion_length,
                    cosine,
                    channel_pdf,
                    transmitted,
                    near_slab_normal,
                    incoming);
                const float mis_dwivedi_far = compute_mis_dwivedi_sampling(
                    distance,
                    extinction,
                    rcp_diffusion_length,
                    cosine,
                    channel_pdf,
                    transmitted,
                    far_slab_normal,
                    incoming);
                const float mis_dwivedi = mix(
                    mis_dwivedi_far,
                    mis_dwivedi_near,
                    bias_strategy_prob);
                transmission *= rcp(mix(
                    mis_dwivedi,
                    mis_classical,
                    classical_sampling_prob));
                bssrdf_sample.m_value *= transmission;
            }

            clamp_in_place(bssrdf_sample.m_value, 0.0f, 2.0f);

            if (n_iteration == 1)
                bssrdf_sample.m_value *= values->m_zero_scattering_weight;
            if (n_iteration == 2)
                bssrdf_sample.m_value *= values->m_single_scattering_weight;
            else
                bssrdf_sample.m_value *= values->m_multiple_scattering_weight;

            bssrdf_sample.m_brdf = m_brdf;
            bssrdf_sample.m_brdf_data = &m_brdf_data;
            bssrdf_sample.m_incoming_point.flip_side();

            // Sample the BSDF at the incoming point.
            bsdf_sample.m_shading_point = &bssrdf_sample.m_incoming_point;
            bsdf_sample.m_geometric_normal = Vector3f(bssrdf_sample.m_incoming_point.get_geometric_normal());
            bsdf_sample.m_shading_basis = Basis3f(bssrdf_sample.m_incoming_point.get_shading_basis());
            bsdf_sample.m_outgoing = Dual3f(bsdf_sample.m_geometric_normal);      // chosen arbitrarily (no outgoing direction at the incoming point)
            bssrdf_sample.m_brdf->sample(
                sampling_context,
                bssrdf_sample.m_brdf_data,
                false,
                true,
                ScatteringMode::All,
                bsdf_sample);

            return true;
        }

        static float compute_transmission_classical_sampling(
            const float         distance,
            const Spectrum&     extinction,
            const Spectrum&     channel_pdf,
            const bool          transmitted,
            Spectrum&           transmission)
        {
            float mis_base = 0.0f;
            for (size_t i = 0, e = Spectrum::size(); i < e; ++i)
            {
                const float x = -distance * extinction[i];
                assert(FP<float>::is_finite(x));
                transmission[i] = std::exp(x);
                if (!transmitted) transmission[i] *= extinction[i];

                mis_base += transmission[i] * channel_pdf[i];
            }
            return mis_base;
        }

        static float compute_mis_dwivedi_sampling(
            const float         distance,
            const Spectrum&     extinction,
            const Spectrum&     rcp_diffusion_length,
            const float         cosine,
            const Spectrum&     channel_pdf,
            const bool          transmitted,
            const Vector3f&     slab_normal,
            const Vector3f&     incoming)
        {
            float mis_base = 0.0f;
            for (size_t i = 0, e = Spectrum::size(); i < e; ++i)
            {
                const float effective_extinction = extinction[i] * (1.0f - cosine * rcp_diffusion_length[i]);
                const float x = -distance * effective_extinction;
                assert(FP<float>::is_finite(x));
                const float distance_prob = (transmitted ? 1.0f : effective_extinction) * std::exp(x);

                DwivediPhaseFunction phase_function(rcp(rcp_diffusion_length[i]));
                const float direction_prob = phase_function.evaluate(slab_normal, incoming);

                mis_base +=
                    distance_prob *
                    channel_pdf[i] *
                    direction_prob;
            }
            return mis_base * FourPi<float>();
        }

        void evaluate(
            const void*             data,
            const ShadingPoint&     outgoing_point,
            const Vector3f&         outgoing_dir,
            const ShadingPoint&     incoming_point,
            const Vector3f&         incoming_dir,
            Spectrum&               value) const override
        {
            const RandomWalkBSSRDFInputValues* values =
                static_cast<const RandomWalkBSSRDFInputValues*>(data);

            throw ExceptionNotImplemented();
        }

        const BSDF*                     m_brdf;
        LambertianBRDFInputValues       m_brdf_data;
    };
}


//
// RandomWalkBSSRDFFactory class implementation.
//

void RandomWalkBSSRDFFactory::release()
{
    delete this;
}

const char* RandomWalkBSSRDFFactory::get_model() const
{
    return Model;
}

Dictionary RandomWalkBSSRDFFactory::get_model_metadata() const
{
    return
        Dictionary()
            .insert("name", Model)
            .insert("label", "Random-Walk BSSRDF");
}

DictionaryArray RandomWalkBSSRDFFactory::get_input_metadata() const
{
    DictionaryArray metadata;

    metadata.push_back(
        Dictionary()
            .insert("name", "weight")
            .insert("label", "Weight")
            .insert("type", "colormap")
            .insert("entity_types",
                Dictionary().insert("texture_instance", "Textures"))
            .insert("use", "optional")
            .insert("default", "1.0"));

    metadata.push_back(
        Dictionary()
            .insert("name", "reflectance")
            .insert("label", "Diffuse Surface Reflectance")
            .insert("type", "colormap")
            .insert("entity_types",
                Dictionary()
                    .insert("color", "Colors")
                    .insert("texture_instance", "Textures"))
            .insert("use", "required")
            .insert("default", "0.5"));

    metadata.push_back(
        Dictionary()
            .insert("name", "reflectance_multiplier")
            .insert("label", "Diffuse Surface Reflectance Multiplier")
            .insert("type", "colormap")
            .insert("entity_types",
                Dictionary().insert("texture_instance", "Textures"))
            .insert("use", "optional")
            .insert("default", "1.0"));

    metadata.push_back(
        Dictionary()
            .insert("name", "mfp")
            .insert("label", "Mean Free Path")
            .insert("type", "colormap")
            .insert("entity_types",
                Dictionary()
                    .insert("color", "Colors")
                    .insert("texture_instance", "Textures"))
            .insert("use", "required")
            .insert("default", "0.5"));

    metadata.push_back(
        Dictionary()
            .insert("name", "mfp_multiplier")
            .insert("label", "Mean Free Path Multiplier")
            .insert("type", "colormap")
            .insert("entity_types",
                Dictionary().insert("texture_instance", "Textures"))
            .insert("use", "optional")
            .insert("default", "1.0"));

    metadata.push_back(
        Dictionary()
            .insert("name", "ior")
            .insert("label", "Index of Refraction")
            .insert("type", "numeric")
            .insert("min",
                Dictionary()
                    .insert("value", "1.0")
                    .insert("type", "hard"))
            .insert("max",
                Dictionary()
                    .insert("value", "2.5")
                    .insert("type", "hard"))
            .insert("use", "required")
            .insert("default", "1.3"));

    metadata.push_back(
        Dictionary()
            .insert("name", "fresnel_weight")
            .insert("label", "Fresnel Weight")
            .insert("type", "numeric")
            .insert("min",
                Dictionary()
                    .insert("value", "0.0")
                    .insert("type", "hard"))
            .insert("max",
                Dictionary()
                    .insert("value", "1.0")
                    .insert("type", "hard"))
            .insert("use", "optional")
            .insert("default", "1.0"));

    metadata.push_back(
        Dictionary()
        .insert("name", "zero_scattering_weight")
        .insert("label", "Zero Scattering Weight")
        .insert("type", "numeric")
        .insert("min",
            Dictionary()
            .insert("value", "0.0")
            .insert("type", "hard"))
        .insert("max",
            Dictionary()
            .insert("value", "1.0")
            .insert("type", "hard"))
        .insert("use", "optional")
        .insert("default", "1.0"));

    metadata.push_back(
        Dictionary()
        .insert("name", "single_scattering_weight")
        .insert("label", "Single Scattering Weight")
        .insert("type", "numeric")
        .insert("min",
            Dictionary()
            .insert("value", "0.0")
            .insert("type", "hard"))
        .insert("max",
            Dictionary()
            .insert("value", "1.0")
            .insert("type", "hard"))
        .insert("use", "optional")
        .insert("default", "1.0"));

    metadata.push_back(
        Dictionary()
        .insert("name", "multiple_scattering_weight")
        .insert("label", "Multiple Scattering Weight")
        .insert("type", "numeric")
        .insert("min",
            Dictionary()
            .insert("value", "0.0")
            .insert("type", "hard"))
        .insert("max",
            Dictionary()
            .insert("value", "1.0")
            .insert("type", "hard"))
        .insert("use", "optional")
        .insert("default", "1.0"));

    return metadata;
}

auto_release_ptr<BSSRDF> RandomWalkBSSRDFFactory::create(
    const char*         name,
    const ParamArray&   params) const
{
    return auto_release_ptr<BSSRDF>(new RandomWalkBSSRDF(name, params));
}

}   // namespace renderer
