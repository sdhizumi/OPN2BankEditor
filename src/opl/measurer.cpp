/*
 * OPL Bank Editor by Wohlstand, a free tool for music bank editing
 * Copyright (c) 2016-2017 Vitaly Novichkov <admin@wohlnet.ru>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef IS_QT_4
#include <QtConcurrent/QtConcurrent>
#include <QFuture>
#endif
#include <QQueue>
#include <QProgressDialog>

#include <cmath>

#include "measurer.h"
#include "generator.h"

//Measurer is always needs for emulator
#include "Ym2612_Emu.h"

struct DurationInfo
{
    uint64_t    peak_amplitude_time;
    double      peak_amplitude_value;
    double      quarter_amplitude_time;
    double      begin_amplitude;
    double      interval;
    double      keyoff_out_time;
    int64_t     ms_sound_kon;
    int64_t     ms_sound_koff;
    bool        nosound;
    uint8_t     padding[7];
};

struct ChipEmulator
{
    Ym2612_Emu opl;
    void setRate(uint32_t rate)
    {
        opl.set_rate(rate, 7670454.0);
    }
    void WRITE_REG(uint8_t port, uint8_t address, uint8_t byte)
    {
        switch(port)
        {
        case 0:
            opl.write0(address, byte);
            break;
        case 1:
            opl.write1(address, byte);
            break;
        }
    }
};

static void MeasureDurations(FmBank::Instrument *in_p)
{
    FmBank::Instrument &in = *in_p;
    std::vector<int16_t> stereoSampleBuf;

    const unsigned rate = 44100;
    const unsigned interval             = 150;
    const unsigned samples_per_interval = rate / interval;
    const int notenum =
        in.percNoteNum < 20 ? (44 + in.percNoteNum) :
                            in.percNoteNum >= 128 ? (44 + 128 - in.percNoteNum) : in.percNoteNum;
    ChipEmulator opn;

    opn.setRate(rate);
    opn.WRITE_REG(0, 0x22, 0x00);   //LFO off
    opn.WRITE_REG(0, 0x27, 0x0 );   //Channel 3 mode normal

    //Shut up all channels
    opn.WRITE_REG(0, 0x28, 0x00 );   //Note Off 0 channel
    opn.WRITE_REG(0, 0x28, 0x01 );   //Note Off 1 channel
    opn.WRITE_REG(0, 0x28, 0x02 );   //Note Off 2 channel
    opn.WRITE_REG(0, 0x28, 0x04 );   //Note Off 3 channel
    opn.WRITE_REG(0, 0x28, 0x05 );   //Note Off 4 channel
    opn.WRITE_REG(0, 0x28, 0x06 );   //Note Off 5 channel

    //Disable DAC
    opn.WRITE_REG(0, 0x2B, 0x0 );   //DAC off

    OPN_PatchSetup patch;

    for(int op = 0; op < 4; op++)
    {
        patch.OPS[op].data[0] = in.getRegDUMUL(op);
        patch.OPS[op].data[1] = in.getRegLevel(op);
        patch.OPS[op].data[2] = in.getRegRSAt(op);
        patch.OPS[op].data[3] = in.getRegAMD1(op);
        patch.OPS[op].data[4] = in.getRegD2(op);
        patch.OPS[op].data[5] = in.getRegSysRel(op);
        patch.OPS[op].data[6] = in.getRegSsgEg(op);
    }
    patch.fbalg    = in.getRegFbAlg();
    patch.lfosens  = in.getRegLfoSens();
    patch.finetune = static_cast<int8_t>(in.note_offset1);
    patch.tone     = 0;

    uint32_t c = 0;
    uint8_t port = (c <= 2) ? 0 : 1;
    uint8_t cc   = c % 3;

    for(uint8_t op = 0; op < 4; op++)
    {
        opn.WRITE_REG(port, 0x30 + (op * 4) + cc, patch.OPS[op].data[0]);
        opn.WRITE_REG(port, 0x40 + (op * 4) + cc, patch.OPS[op].data[1]);
        opn.WRITE_REG(port, 0x50 + (op * 4) + cc, patch.OPS[op].data[2]);
        opn.WRITE_REG(port, 0x60 + (op * 4) + cc, patch.OPS[op].data[3]);
        opn.WRITE_REG(port, 0x70 + (op * 4) + cc, patch.OPS[op].data[4]);
        opn.WRITE_REG(port, 0x80 + (op * 4) + cc, patch.OPS[op].data[5]);
        opn.WRITE_REG(port, 0x90 + (op * 4) + cc, patch.OPS[op].data[6]);
    }
    opn.WRITE_REG(port, 0xB0 + cc, patch.fbalg);
    opn.WRITE_REG(port, 0xB4 + cc,  0xC0);


    {
        double hertz = 321.88557 * std::exp(0.057762265 * (notenum + in.note_offset1));
        uint16_t x2 = 0x0000;
        if(hertz < 0 || hertz > 262143)
        {
            std::fprintf(stderr, "MEASURER WARNING: Why does note %d + finetune %d produce hertz %g?          \n",
                         notenum, in.note_offset1, hertz);
            hertz = 262143;
        }

        while(hertz >= 2047.5)
        {
            hertz /= 2.0;    // Calculate octave
            x2 += 0x800;
        }

        x2 += static_cast<uint32_t>(hertz + 0.5);

        // Keyon the note
        opn.WRITE_REG(port, 0xA4 + cc, (x2>>8) & 0xFF);//Set frequency and octave
        opn.WRITE_REG(port, 0xA0 + cc,  x2 & 0xFF);

        opn.WRITE_REG(0, 0x28, 0xF0 + uint8_t((c <= 2) ? c : c + 1));
    }

    const unsigned max_on  = 40;
    const unsigned max_off = 60;

    const double min_coefficient = 0.005;

    // For up to 40 seconds, measure mean amplitude.
    std::vector<double> amplitudecurve_on;
    double highest_sofar = 0;
    for(unsigned period = 0; period < max_on * interval; ++period)
    {
        stereoSampleBuf.clear();
        stereoSampleBuf.resize(samples_per_interval * 2, 0);

        opn.opl.run(samples_per_interval, stereoSampleBuf.data());

        double mean = 0.0;
        for(unsigned long c = 0; c < samples_per_interval; ++c)
            mean += stereoSampleBuf[c * 2];
        mean /= samples_per_interval;
        double std_deviation = 0;
        for(unsigned long c = 0; c < samples_per_interval; ++c)
        {
            double diff = (stereoSampleBuf[c * 2] - mean);
            std_deviation += diff * diff;
        }
        std_deviation = std::sqrt(std_deviation / samples_per_interval);
        amplitudecurve_on.push_back(std_deviation);
        if(std_deviation > highest_sofar)
            highest_sofar = std_deviation;

        if(period > 6 * interval && std_deviation < highest_sofar * min_coefficient)
            break;
    }

    // Keyoff the note
    {
        uint8_t cc = static_cast<uint8_t>(c % 6);
        opn.WRITE_REG(0, 0x28, (c <= 2) ? cc : cc + 1);
    }

    // Now, for up to 60 seconds, measure mean amplitude.
    std::vector<double> amplitudecurve_off;
    for(unsigned period = 0; period < max_off * interval; ++period)
    {
        stereoSampleBuf.clear();
        stereoSampleBuf.resize(samples_per_interval * 2);

        opn.opl.run(samples_per_interval, stereoSampleBuf.data());

        double mean = 0.0;
        for(unsigned long c = 0; c < samples_per_interval; ++c)
            mean += stereoSampleBuf[c * 2];
        mean /= samples_per_interval;
        double std_deviation = 0;
        for(unsigned long c = 0; c < samples_per_interval; ++c)
        {
            double diff = (stereoSampleBuf[c * 2] - mean);
            std_deviation += diff * diff;
        }
        std_deviation = std::sqrt(std_deviation / samples_per_interval);
        amplitudecurve_off.push_back(std_deviation);

        if(std_deviation < highest_sofar * min_coefficient)
            break;
    }

    /* Analyze the results */
    double begin_amplitude        = amplitudecurve_on[0];
    double peak_amplitude_value   = begin_amplitude;
    size_t peak_amplitude_time    = 0;
    size_t quarter_amplitude_time = amplitudecurve_on.size();
    size_t keyoff_out_time        = 0;

    for(size_t a = 1; a < amplitudecurve_on.size(); ++a)
    {
        if(amplitudecurve_on[a] > peak_amplitude_value)
        {
            peak_amplitude_value = amplitudecurve_on[a];
            peak_amplitude_time  = a;
        }
    }
    for(size_t a = peak_amplitude_time; a < amplitudecurve_on.size(); ++a)
    {
        if(amplitudecurve_on[a] <= peak_amplitude_value * min_coefficient)
        {
            quarter_amplitude_time = a;
            break;
        }
    }
    for(size_t a = 0; a < amplitudecurve_off.size(); ++a)
    {
        if(amplitudecurve_off[a] <= peak_amplitude_value * min_coefficient)
        {
            keyoff_out_time = a;
            break;
        }
    }

    if(keyoff_out_time == 0 && amplitudecurve_on.back() < peak_amplitude_value * min_coefficient)
        keyoff_out_time = quarter_amplitude_time;

    DurationInfo result;
    result.peak_amplitude_time = peak_amplitude_time;
    result.peak_amplitude_value = peak_amplitude_value;
    result.begin_amplitude = begin_amplitude;
    result.quarter_amplitude_time = (double)quarter_amplitude_time;
    result.keyoff_out_time = (double)keyoff_out_time;

    result.ms_sound_kon  = (int64_t)(quarter_amplitude_time * 1000.0 / interval);
    result.ms_sound_koff = (int64_t)(keyoff_out_time        * 1000.0 / interval);
    result.nosound = (peak_amplitude_value < 0.5);

    in.ms_sound_kon = (uint16_t)result.ms_sound_kon;
    in.ms_sound_koff = (uint16_t)result.ms_sound_koff;
}


Measurer::Measurer(QWidget *parent) :
    QObject(parent),
    m_parentWindow(parent)
{}

Measurer::~Measurer()
{}

bool Measurer::doMeasurement(FmBank &bank, FmBank &bankBackup)
{
    QQueue<FmBank::Instrument *> tasks;

    int i = 0;
    for(i = 0; i < bank.Ins_Melodic_box.size() && i < bankBackup.Ins_Melodic_box.size(); i++)
    {
        FmBank::Instrument &ins1 = bank.Ins_Melodic_box[i];
        //FmBank::Instrument &ins2 = bankBackup.Ins_Melodic_box[i];
        //if((ins1.ms_sound_kon == 0) || (memcmp(&ins1, &ins2, sizeof(FmBank::Instrument)) != 0))
            tasks.enqueue(&ins1);
    }
    for(; i < bank.Ins_Melodic_box.size(); i++)
        tasks.enqueue(&bank.Ins_Melodic_box[i]);

    for(i = 0; i < bank.Ins_Percussion_box.size() && i < bankBackup.Ins_Percussion_box.size(); i++)
    {
        FmBank::Instrument &ins1 = bank.Ins_Percussion_box[i];
        //FmBank::Instrument &ins2 = bankBackup.Ins_Percussion_box[i];
        //if((ins1.ms_sound_kon == 0) || (memcmp(&ins1, &ins2, sizeof(FmBank::Instrument)) != 0))
            tasks.enqueue(&ins1);
    }
    for(; i < bank.Ins_Percussion_box.size(); i++)
        tasks.enqueue(&bank.Ins_Percussion_box[i]);

    if(tasks.isEmpty())
        return true;// Nothing to do! :)

    QProgressDialog m_progressBox(m_parentWindow);
    m_progressBox.setWindowModality(Qt::WindowModal);
    m_progressBox.setWindowTitle(tr("Sounding delay calculaion"));
    m_progressBox.setLabelText(tr("Please wait..."));

    #ifndef IS_QT_4
    QFutureWatcher<void> watcher;
    watcher.connect(&m_progressBox, SIGNAL(canceled()), &watcher, SLOT(cancel()));
    watcher.connect(&watcher, SIGNAL(progressRangeChanged(int,int)), &m_progressBox, SLOT(setRange(int,int)));
    watcher.connect(&watcher, SIGNAL(progressValueChanged(int)), &m_progressBox, SLOT(setValue(int)));

    watcher.setFuture(QtConcurrent::map(tasks, &MeasureDurations));

    m_progressBox.exec();
    watcher.waitForFinished();

    tasks.clear();
    return !watcher.isCanceled();

    #else
    m_progressBox.setMaximum(tasks.size());
    m_progressBox.setValue(0);
    int count = 0;
    foreach(FmBank::Instrument *ins, tasks)
    {
        MeasureDurations(ins);
        m_progressBox.setValue(++count);
        if(m_progressBox.wasCanceled())
            return false;
    }
    return true;
    #endif
}

bool Measurer::doMeasurement(FmBank::Instrument &instrument)
{
    QProgressDialog m_progressBox(m_parentWindow);
    m_progressBox.setWindowModality(Qt::WindowModal);
    m_progressBox.setWindowTitle(tr("Sounding delay calculaion"));
    m_progressBox.setLabelText(tr("Please wait..."));

    #ifndef IS_QT_4
    QFutureWatcher<void> watcher;
    watcher.connect(&m_progressBox, SIGNAL(canceled()), &watcher, SLOT(cancel()));
    watcher.connect(&watcher, SIGNAL(progressRangeChanged(int,int)), &m_progressBox, SLOT(setRange(int,int)));
    watcher.connect(&watcher, SIGNAL(progressValueChanged(int)), &m_progressBox, SLOT(setValue(int)));

    watcher.setFuture(QtConcurrent::run(&MeasureDurations, &instrument));

    m_progressBox.exec();
    watcher.waitForFinished();

    return !watcher.isCanceled();

    #else
    m_progressBox.show();
    MeasureDurations(&instrument);
    return true;
    #endif
}
