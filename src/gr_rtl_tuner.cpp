// Copyright (C) 2019, Jaguar Land Rover
// This program is licensed under the terms and conditions of the
// Mozilla Public License, version 2.0.  The full text of the
// Mozilla Public License is at https://www.mozilla.org/MPL/2.0/
//
// Author: Jason Anderson (jander10@jaguarlandrover.com)

#include "gr_rtl_tuner.h"

#include <chrono>
#include <ctime>

#include <thread>
#include <mutex>
#include <numeric>

#include <iostream> // Debugging only

#include "gnuradio/top_block.h"
#include "osmosdr/source.h"
#include "gnuradio/filter/rational_resampler_base_ccc.h"
#include "gnuradio/filter/rational_resampler_base_fff.h"
#include "gnuradio/filter/fir_filter_ccf.h"
#include "gnuradio/filter/firdes.h"
#include "gnuradio/audio/sink.h"
#include "gnuradio/hier_block2.h"
#include "gnuradio/gr_complex.h"
#include "gnuradio/analog/quadrature_demod_cf.h"
#include "gnuradio/filter/iir_filter_ffd.h"
#include "gnuradio/filter/fir_filter_fff.h"
#include "gnuradio/blocks/wavfile_source.h"
#include "gnuradio/analog/probe_avg_mag_sqrd_c.h"
#include "gr_wfmrcv.h"

const unsigned int MAX_FM_STATIONS = 100;  // maximum number of poossible stations in FM band that we could find

// Structure to hold smart pointers to flowgraph blocks for the rtl sdr tuner
struct rtl_ctx {
    gr::top_block_sptr top_block;
    osmosdr::source::sptr rtl_source;
    gr::analog::probe_avg_mag_sqrd_c::sptr avg_magnitude;
    double station_list[MAX_FM_STATIONS];
    unsigned int station_list_len;
    std::mutex station_list_mtx;
};

// Sets the FM center frequency for the given tuner
// Part of the external C API
// @param tuner Pointer to the tuner context
// @param freq Frequency in megahertz e.g. 105.9
void rtl_set_fm(rtl_ctx_t* tuner, double freq)
{
    tuner->rtl_source->set_center_freq(freq * 1e6);
}

// Gets the current center FM frequency that the tuner is set to
// Part of the external C API
// @param tuner Pointer to the tuner context
// @returns currently tuned frequency in MHz
double rtl_get_fm(rtl_ctx_t* tuner)
{
    return tuner->rtl_source->get_center_freq() / 1e6;
}

// Iterates through the FM band, measures signal strength of each station, and populates station list
// Note: this is a long-running function and should be run in the background
// @param tuner Pointer to the tuner context
void scan_fm_stations(rtl_ctx_t* tuner) {
    // TODO: these constants will likely need adjustments depending on the setup, should be sampled/benchmarked somehow
    const unsigned int SWITCH_DELAY_MS = 1000; // time to wait between switching stations and measuring the signal strength
    const unsigned int MEASURE_MS = 200;       // time to sample the signal strength of each frequency (takes the highest sample)
    const double POWER_THRESHOLD = 0.0002;     // minimum power a signal must have to be considered a valid channel
    unsigned int found_stations = 0;
    double stations_out[MAX_FM_STATIONS];
    printf("Starting scan\n");
    for (double freq = 87.9; freq <= 107.9; freq += 0.2) {
        rtl_set_fm(tuner, freq);
        std::this_thread::sleep_for(std::chrono::milliseconds(SWITCH_DELAY_MS));
        double sample_sum = 0;
        unsigned int num_samples = 0;
        std::chrono::high_resolution_clock::time_point start = std::chrono::high_resolution_clock::now();
        while (std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::high_resolution_clock::now() - start).count() < MEASURE_MS) {
            sample_sum += tuner->avg_magnitude->level();
            ++num_samples;
        }
        if (num_samples > 0) {
            double sample_avg = sample_sum / num_samples;
            if (sample_avg > POWER_THRESHOLD) {
                printf("\tFound station: %f, strength: %f\n", freq, sample_avg);
                stations_out[found_stations++] = freq;
            }
        }
    }
    {
        std::lock_guard<std::mutex> lock(tuner->station_list_mtx);
        memcpy(tuner->station_list, stations_out, sizeof(double) * found_stations);
        tuner->station_list_len = found_stations;
    }
    printf("Finished scan\n");
}

// Gets the most recent station list measured by scan_fm_stations
// Part of the external C API
// @param tuner Pointer to the tuner context
unsigned int rtl_get_fm_stations(rtl_ctx_t* tuner, double* stations_out) {
    // TODO: scan_fm_stations should happen in the background once the tuner supports two antennas
    scan_fm_stations(tuner);
    unsigned int stations_out_len = 0;
    {
        std::lock_guard<std::mutex> lock(tuner->station_list_mtx);
        memcpy(stations_out, tuner->station_list, sizeof(double) * tuner->station_list_len);
        stations_out_len = tuner->station_list_len;
    }
    return stations_out_len;
}

// Does all of the heavy listing setting up a flowgraph for an rtl_sdr radio source
// @parame context Reference to the tuner context.  This is a struct and not a class because
// the rtl_ctx is typedefed to an opaque type in the header to allow compatibility with C
void create_fm_device(rtl_ctx &context, audio_sink_t sink_type)
{
    int mltpl = 1e6;
    int volume = 20;
    int transition = 1e6;
    int samp_rate = 2e6;
    int quadrature = 500e3;
    double freq = 101.9;
    int cutoff = 100e3;
    int audio_dec = 10;
    int max_dev = 75e3;

    gr::top_block_sptr tb = gr::make_top_block("top");
    osmosdr::source::sptr rtlsrc = osmosdr::source::make("numchan=1 rtl=0");

    context.top_block = tb;
    context.rtl_source = rtlsrc;

    //rtlsrc->set_time_now(std::chrono::system_clock::to_time_t(std::chrono::system_clock::now()), osmosdr::ALL_MBOARDS);
    rtlsrc->set_sample_rate(samp_rate);
    rtlsrc->set_center_freq(freq * 1e6);
    rtlsrc->set_freq_corr(0, 0);
    rtlsrc->set_dc_offset_mode(2, 0);
    rtlsrc->set_iq_balance_mode(1, 0);
    rtlsrc->set_gain_mode(false, 0);
    rtlsrc->set_gain(20, 0);
    rtlsrc->set_if_gain(20, 0);
    rtlsrc->set_bb_gain(20, 0);
    rtlsrc->set_antenna("", 0);
    rtlsrc->set_bandwidth(0, 0);

    int dec1 = int(samp_rate / quadrature);
    printf("dec1: %d \n", dec1);

    gr::filter::rational_resampler_base_ccc::sptr rresamp1;

    double d = 1.0;

    double inter = floor(1.0 / d);
    double deci = floor(double(dec1) / d);

    printf("inter: %f \n", inter);

    double fractional_bw = 0.4;
    double beta = 7.0;
    double halfband = 0.5;
    double rate = 1.0 / deci;
    double trans_width = 0.0;
    double mid_transition_band = 0.0;

    if (rate >= 1.0)
    {
        trans_width = halfband - fractional_bw;
        mid_transition_band = halfband - trans_width / 2.0;
        printf("mid_transition_band: %f \n", mid_transition_band);
    }
    else
    {
        trans_width = rate * (halfband - fractional_bw);
        printf("trans_width: %f \n", trans_width);
        mid_transition_band = rate * halfband - trans_width / 2.0;
        printf("mid_transition_band2: %f \n", mid_transition_band);
    }

    std::vector<float> taps1 = gr::filter::firdes::low_pass(
        (inter),
        (inter),
        (mid_transition_band),
        (trans_width),
        gr::filter::firdes::WIN_KAISER,
        (beta));

    std::vector<gr_complex> taps_comp;

    std::vector<float>::iterator iter = taps1.begin();
    for (; iter != taps1.end(); ++iter)
    {
        taps_comp.push_back(gr_complex(*iter));
    }

    rresamp1 = gr::filter::rational_resampler_base_ccc::make(
        1.0,
        dec1,
        taps_comp);

    int dec2 = int(quadrature / 1e3 / audio_dec);
    printf("dec2: %d \n", dec2);
    gr::filter::rational_resampler_base_fff::sptr rresamp0;

    d = 2.0;

    inter = floor(48.0 / d);
    printf("inter: %f \n", inter);

    deci = floor(dec2 / d);

    fractional_bw = 0.4;
    beta = 7.0;
    halfband = 0.5;
    rate = 1 / deci;
    trans_width = 0.0;
    mid_transition_band = 0.0;

    if (rate >= 1.0)
    {
        trans_width = halfband - fractional_bw;
        mid_transition_band = halfband - trans_width / 2.0;
        printf("mid_transition_band: %f \n", mid_transition_band);
    }
    else
    {
        trans_width = rate * (halfband - fractional_bw);
        printf("trans_width: %f \n", trans_width);
        mid_transition_band = rate * halfband - trans_width / 2.0;
        printf("mid_transition_band2: %f \n", mid_transition_band);
    }

    std::vector<float> taps = gr::filter::firdes::low_pass(
        (inter),
        (inter),
        float(mid_transition_band),
        float(trans_width),
        gr::filter::firdes::WIN_KAISER,
        float(beta));

    rresamp0 = gr::filter::rational_resampler_base_fff::make(
        48.0,
        dec2,
        taps);

    gr::filter::fir_filter_ccf::sptr lp0 = gr::filter::fir_filter_ccf::make(1,
                                                                            gr::filter::firdes::low_pass(
                                                                                1,
                                                                                samp_rate,
                                                                                cutoff,
                                                                                transition));

    gr::audio::sink::sptr audsink = gr::audio::sink::make(44100);

    gr::analog::wfmrcv::sptr wfm = gr::analog::wfmrcv::make(
      quadrature,
      audio_dec
    );


    gr::hier_block2_sptr wfmrcv = gr::make_hier_block2(
        "wfm_rcv",
        gr::io_signature::make(1, 1, sizeof(gr_complex)),
        gr::io_signature::make(1, 1, sizeof(float)));

    float fm_demod_gain = quadrature / (2 * M_PI * max_dev);
    float audio_rate = quadrature / audio_dec;

    gr::analog::probe_avg_mag_sqrd_c::sptr mag_probe = gr::analog::probe_avg_mag_sqrd_c::make(0.0);
    context.avg_magnitude = mag_probe;

    /*gr::blocks::wavfile_source::sptr filesink = gr::blocks::wavfile_source::make(
        "/home/jlruser/yocto/qcom-linux-guest/apps_proc/audio/mm-audio/audio-listen/sva/res/raw/succeed.wav"
    );*/

    tb->connect(
        rtlsrc, 0,
        rresamp1, 0);

    tb->connect(
        rresamp1, 0,
        lp0, 0);

    tb->connect(
        lp0, 0,
        wfm, 0);

    tb->connect(
        lp0, 0,
        mag_probe, 0);

    tb->connect(
        wfm, 0,
        rresamp0, 0);

    tb->connect(
        rresamp0, 0,
        audsink, 0);
}

// Creates and allocates an instance of an rtl tuner context.
// Part of the external API
// @return A pointer to a newly allocated tuner context
rtl_ctx_t* rtl_create_tuner(audio_sink_t sink_type)
{
    rtl_ctx_t* tuner_ctx = new rtl_ctx_t;
    if (tuner_ctx == NULL)
    {
        printf("Error: rtl_create_tuner - out of memory\n");
        return NULL;
    }
    create_fm_device(*tuner_ctx, sink_type);

    return tuner_ctx;
}

// Shuts down and destroyes a tuner context
// Part of the external API
// @param The tuner context
void rtl_destroy_tuner(rtl_ctx_t* tuner)
{
    if (tuner == NULL)
    {
        printf("Error: rtl_destroy_tuner - null pointer\n");
        return;
    }
    tuner->top_block->stop();
    tuner->top_block.reset();
    delete tuner;
}

// Starts up a tuner context running.  Blocks forever since the flowgraph never terminates.
// The assumption is that the caller will start this function on a dedicated thread because it blocks
// Part of the external API
// @param tuner The tuner context
void rtl_start_fm(rtl_ctx_t* tuner)
{
    if (tuner == NULL)
    {
        printf("Error: rtl_start_fm - no tuner specified\n");
        return;
    }
    tuner->top_block->start();
    tuner->top_block->wait();
}

// Stops a running flowgraph
// Part of the external API
// @param tuner The tuner context
void rtl_stop_fm(rtl_ctx_t* tuner)
{
    if (tuner == NULL)
    {
        printf("Error: rtl_stop_fm - no tuner specified\n");
        return;
    }

    tuner->top_block->stop();
}
