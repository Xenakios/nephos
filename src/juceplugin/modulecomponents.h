#pragma once

#include "PluginProcessor.h"
#include "juce_core/juce_core.h"
#include "juce_events/juce_events.h"
#include "juce_graphics/juce_graphics.h"
#include "juce_gui_basics/juce_gui_basics.h"
#include "text/choc_JSON.h"
#include "xap_slider.h"
#include "dropdowncomponent.h"
#include <exception>
#include <random>

class VolumeEnvelopeComponent : public juce::Component
{
  public:
    VolumeEnvelopeComponent(ToneGranulator *gr, bool auxenv) : granul(gr), auxenvmode(auxenv)
    {
        curvepath.preallocateSpace(512);
        rng.seed(65537, 90004);
    }
    xenakios::Xoroshiro128Plus rng;
    std::string lastError;
    enum GenMode
    {
        GM_RESET,
        GM_RAMPUP,
        GM_RAMPDOWN,
        GM_RANDOM,
        GM_CLIPBOARD
    };
    enum TransformMode
    {
        TM_Reverse,
        TM_RotateLeft,
        TM_RotateRight,
        TM_Sort,
        TM_ApplyEnvelope
    };
    void generate_steps(GenMode mode);
    void transform_steps(TransformMode mode);
    void set_interpolation_mode(int m)
    {
        granul->set_aux_envelope_interpolation_mode(m);
        juce::Timer::callAfterDelay(100, [this]() { repaint(); });
    }
    void mouseDown(const juce::MouseEvent &ev) override;
    juce::PopupMenu generate_presets_menu();
    void mouseWheelMove(const juce::MouseEvent &event,
                        const juce::MouseWheelDetails &wheel) override
    {
        if (!auxenvmode)
            return;
        auto numsteps = SimpleEnvelope<false>::maxnumsteps;
        int stepindex = numsteps / (float)getWidth() * event.x;
        if (stepindex >= 0 && stepindex < numsteps)
        {
            float delta = wheel.deltaY * 0.2;
            auto &auxenv = granul->voiceaux_envelope;
            float val = std::clamp(auxenv.steps[stepindex] + delta, -1.0f, 1.0f);
            StepModSource::Message msg;
            msg.opcode = StepModSource::Message::OP_SETSTEP;
            msg.fval0 = val;
            msg.dest = 1000;
            msg.ival0 = stepindex;
            granul->fifo.push(msg);
        }
        repaint();
    }
    void paint(juce::Graphics &g) override;

    void updateIfNeeded()
    {
        if (!auxenvmode)
        {
            int curvestart = *granul->idtoparvalptr[ToneGranulator::PAR_VOLENVEASINGSTART];
            int curveend = *granul->idtoparvalptr[ToneGranulator::PAR_VOLENVEASINGEND];
            float curvemorph = *granul->idtoparvalptr[ToneGranulator::PAR_ENVMORPH];
            if (priorstartcurve != curvestart || priorendcurve != curveend ||
                priormorph != curvemorph)
            {
                priorstartcurve = curvestart;
                priorendcurve = curveend;
                priormorph = curvemorph;
                repaint();
                // DBG(priorstartcurve << " " << priorendcurve << " " << priormorph);
            }
        }
        else
        {
            float warp = granul->auxenvwarpmodulated;
            float depth = granul->auxenvdepthpmodulated / 12.0f;
            if (warp != priorauxwarp || depth != priorauxamount)
            {
                priorauxwarp = warp;
                priorauxamount = depth;
                repaint();
            }
        }
    }
    ToneGranulator *granul = nullptr;
    juce::Path curvepath;
    int priorstartcurve = 0;
    int priorendcurve = 0;
    float priormorph = 0.0f;
    float priorauxwarp = 0.0f;
    float priorauxamount = 0.0f;
    bool auxenvmode = false;
};

inline void initSlider(AudioPluginAudioProcessor &processor, juce::Component &parentComponent,
                       XapSlider &slid)
{
    parentComponent.addAndMakeVisible(slid);
    slid.OnValueChanged = [&processor, &slid]() {
        ParameterMessage msg;
        msg.id = slid.getParameterMetaData().id;
        msg.value = slid.getValue();
        processor.params_from_gui_fifo.push(msg);
    };
}

class OscTypeComponent : public juce::Component, public juce::Timer
{
  public:
    AudioPluginAudioProcessor &processorRef;
    int priorOscType = -1;
    std::vector<std::string> waveshortnames{{"SIN", "SEMI", "TRI", "SAW", "SQR", "FM", "NOIS"}};
    std::vector<juce::Image> waveimages;
    juce::Image drawWaveImage(std::function<float(float)> func)
    {
        juce::Image img{juce::Image::ARGB, 50, 50, true};
        juce::Graphics g(img);
        g.fillAll(juce::Colours::transparentWhite);
        juce::Path p;
        for (int i = 0; i < img.getWidth(); ++i)
        {
            float x = juce::jmap<float>(i, 0, img.getWidth() - 1, 0.0f, 1.0f);
            float y = (1.0 - func(x)) * 0.95 + 0.025;
            y = y * img.getHeight();
            if (i == 0)
            {
                p.startNewSubPath(0.0f, y);
            }
            else
            {
                p.lineTo(i, y);
            }
        }
        g.setColour(juce::Colours::white);
        g.strokePath(p, juce::PathStrokeType(2.0f));
        return img;
    }
    OscTypeComponent(AudioPluginAudioProcessor &p) : processorRef(p)
    {
        waveimages.push_back(
            drawWaveImage([](float x) { return 0.5 + 0.5 * std::sin(M_PI * 2 * x); }));
        waveimages.push_back(
            drawWaveImage([](float x) { return 0.5 + 0.5 * std::sin(M_PI * 1 * x); }));
        waveimages.push_back(drawWaveImage([](float x) {
            if (x < 0.25)
                return juce::jmap<float>(x, 0.0, 0.25, 0.5, 1.0);
            else if (x >= 0.25 && x < 0.75)
                return juce::jmap<float>(x, 0.25, 0.75, 1.0, 0.0);
            return juce::jmap<float>(x, 0.75, 1.0, 0.0, 0.5);
        }));
        waveimages.push_back(drawWaveImage([](float x) { return x; }));
        waveimages.push_back(drawWaveImage([](float x) {
            if (x < 0.5)
                return 0.0;
            return 1.0;
        }));
        waveimages.push_back(drawWaveImage([](float x) {
            float modulator = 2.0 + 1.0 * std::sin(M_PI * 2 * 1.2 * x);
            return 0.5 + 0.5 * std::sin(2 * M_PI * modulator);
        }));
        std::minstd_rand0 rng;
        std::normal_distribution<float> dist{0.5f, 0.2f};
        waveimages.push_back(
            drawWaveImage([&dist, &rng](float x) { return std::clamp(dist(rng), 0.0f, 1.0f); }));
        startTimerHz(25);
    }
    void timerCallback() override
    {
        int curotype = processorRef.granulator.modulatedOscType;
        if (priorOscType != curotype)
        {
            priorOscType = curotype;
            repaint();
        }
    }
    void paint(juce::Graphics &g) override
    {
        g.fillAll(juce::Colours::black);
        int otypebase = *processorRef.granulator.idtoparvalptr[ToneGranulator::PAR_OSCTYPE];
        float cellw = 50.0f;
        for (int i = 0; i < 7; ++i)
        {
            if (i == otypebase)
                g.setColour(juce::Colours::yellow.darker());
            else
                g.setColour(juce::Colours::darkgrey);
            juce::Rectangle<float> r{cellw * i + 2.0f, 2.0f, cellw - 4.0f, cellw - 4.0f};
            g.fillRect(r);

            g.setColour(juce::Colours::white);
            int mappedtype = processorRef.granulator.osctypemapping[i];
            if (mappedtype >= 0 && mappedtype < waveimages.size())
            {
                g.drawImage(waveimages[mappedtype], cellw * i + 2.0, 2.0f, cellw - 4.0f,
                            cellw - 4.0f, 0, 0, 50, 50);
            }
            if (i == priorOscType)
            {
                g.setColour(juce::Colours::green);
                g.fillRect(cellw * i + 4.0f, 4.0f, 8.0f, 8.0f);
            }
            // g.drawFittedText(waveshortnames[mappedtype], r.getX(), r.getY(), r.getWidth(),
            //                  r.getHeight(), juce::Justification::centred, 1);
        }
    }
    void mouseDown(const juce::MouseEvent &ev) override
    {
        int otype = ev.x / 50.0;
        if (otype >= 0 && otype < 7)
        {
            if (!ev.mods.isRightButtonDown())
            {
                ParameterMessage msg;
                msg.id = ToneGranulator::PAR_OSCTYPE;
                msg.value = otype;
                processorRef.params_from_gui_fifo.push(msg);
            }
            else
            {
                juce::PopupMenu menu;
                for (int i = 0; i < 7; ++i)
                {
                    auto dr = std::make_unique<juce::DrawableImage>(waveimages[i]);
                    menu.addItem(juce::PopupMenu::Item(waveshortnames[i])
                                     .setImage(std::move(dr))
                                     .setID(i + 1)
                                     .setAction([this, otype, i] {
                                         processorRef.granulator.osctypemapping[otype] = i;
                                         repaint();
                                     }));
                }
                menu.showMenuAsync(juce::PopupMenu::Options{}.withStandardItemHeight(50));
            }
        }
        repaint();
    }
};

class OscillatorModuleComponent : public juce::GroupComponent
{
  public:
    AudioPluginAudioProcessor &processorRef;
    OscTypeComponent oscTypeComponent;
    XapSlider oscPitchKnob;
    XapSlider pitchEnvKnob;
    XapSlider pitchEnvWarpKnob;
    XapSlider oscSyncKnob;
    XapSlider oscPWKnob;
    XapSlider oscFMPitchKnob;
    XapSlider oscFMDepthKnob;
    XapSlider oscFMFeedbackKnob;
    XapSlider oscNoiseCorrelationKnob;
    XapSlider oscNoiseModeDrop;
    VolumeEnvelopeComponent pitchEnvelopeComponent;
    juce::TextEditor oscTypeEditor;
    OscillatorModuleComponent(AudioPluginAudioProcessor &p)
        : juce::GroupComponent("", "Oscillator"), processorRef(p), oscTypeComponent(p),
          oscPitchKnob(XapSlider::SS_Knob,
                       *p.granulator.idtoparmetadata[ToneGranulator::PAR_PITCH]),
          pitchEnvKnob(XapSlider::SS_Knob,
                       *p.granulator.idtoparmetadata[ToneGranulator::PAR_AUXENVTOPITCHAMT]),
          pitchEnvWarpKnob(XapSlider::SS_Knob,
                           *p.granulator.idtoparmetadata[ToneGranulator::PAR_AUXENVTIMEWARP]),
          oscSyncKnob(XapSlider::SS_Knob,
                      *p.granulator.idtoparmetadata[ToneGranulator::PAR_OSC_SYNC]),
          oscPWKnob(XapSlider::SS_Knob, *p.granulator.idtoparmetadata[ToneGranulator::PAR_OSC_PW]),
          oscFMPitchKnob(XapSlider::SS_Knob,
                         *p.granulator.idtoparmetadata[ToneGranulator::PAR_FMPITCH]),
          oscFMDepthKnob(XapSlider::SS_Knob,
                         *p.granulator.idtoparmetadata[ToneGranulator::PAR_FMDEPTH]),
          oscFMFeedbackKnob(XapSlider::SS_Knob,
                            *p.granulator.idtoparmetadata[ToneGranulator::PAR_FMFEEDBACK]),
          oscNoiseModeDrop(XapSlider::SS_HorizontalSlider,
                           *p.granulator.idtoparmetadata[ToneGranulator::PAR_NOISEMODE]),
          oscNoiseCorrelationKnob(
              XapSlider::SS_Knob,
              *p.granulator.idtoparmetadata[ToneGranulator::PAR_NOISECORRELATION]),
          pitchEnvelopeComponent(&p.granulator, true)
    {
        addAndMakeVisible(oscTypeComponent);
        initSlider(p, *this, oscPitchKnob);
        initSlider(p, *this, pitchEnvKnob);
        initSlider(p, *this, pitchEnvWarpKnob);
        initSlider(p, *this, oscSyncKnob);
        initSlider(p, *this, oscPWKnob);
        initSlider(p, *this, oscFMPitchKnob);
        initSlider(p, *this, oscFMDepthKnob);
        initSlider(p, *this, oscFMFeedbackKnob);
        initSlider(p, *this, oscNoiseModeDrop);
        oscNoiseModeDrop.dropdownXpercent = 0.3;
        initSlider(p, *this, oscNoiseCorrelationKnob);
        addAndMakeVisible(pitchEnvelopeComponent);
        addChildComponent(oscTypeEditor);
        oscTypeEditor.setBounds(2, 2, 200, 25);
    }
    void resized() override
    {
        oscTypeComponent.setBounds(7, 17, 350, 50);
        oscPitchKnob.setBounds(7, oscTypeComponent.getBottom() + 1, 80, 100);
        pitchEnvKnob.setBounds(oscPitchKnob.getRight() + 2, oscTypeComponent.getBottom() + 1, 80,
                               50);
        pitchEnvWarpKnob.setBounds(oscPitchKnob.getRight() + 2, pitchEnvKnob.getBottom() + 1, 80,
                                   50);
        pitchEnvelopeComponent.setBounds(pitchEnvKnob.getRight() + 2, oscTypeComponent.getBottom(),
                                         150, 150);
        oscSyncKnob.setBounds(pitchEnvelopeComponent.getRight() + 2,
                              oscTypeComponent.getBottom() + 1, 80, 50);
        oscPWKnob.setBounds(pitchEnvelopeComponent.getRight() + 2, oscSyncKnob.getBottom() + 1, 80,
                            50);
        oscFMPitchKnob.setBounds(oscSyncKnob.getRight() + 2, oscTypeComponent.getBottom() + 1, 80,
                                 50);
        oscFMDepthKnob.setBounds(oscFMPitchKnob.getRight() + 2, oscTypeComponent.getBottom() + 1,
                                 80, 50);
        oscFMFeedbackKnob.setBounds(oscFMDepthKnob.getRight() + 2, oscTypeComponent.getBottom() + 1,
                                    80, 50);
        oscNoiseModeDrop.setBounds(oscSyncKnob.getRight() + 2, oscFMPitchKnob.getBottom() + 1, 250,
                                   25);
        oscNoiseCorrelationKnob.setBounds(oscSyncKnob.getRight() + 2,
                                          oscNoiseModeDrop.getBottom() + 1, 80, 50);
    }
};

class StackingModuleComponent : public juce::GroupComponent
{
  public:
    AudioPluginAudioProcessor &processorRef;
    XapSlider countKnob;
    XapSlider lengthKnob;
    XapSlider warpKnob;
    XapSlider pitchRandomKnob;
    XapSlider spatRandomKnob;
    StackingModuleComponent(AudioPluginAudioProcessor &p)
        : juce::GroupComponent("", "Stacking"), processorRef(p),
          countKnob(XapSlider::SS_Knob,
                    *p.granulator.idtoparmetadata[ToneGranulator::PAR_STACKCOUNT]),
          lengthKnob(XapSlider::SS_Knob,
                     *p.granulator.idtoparmetadata[ToneGranulator::PAR_STACKTIMESPAN]),
          warpKnob(XapSlider::SS_Knob,
                   *p.granulator.idtoparmetadata[ToneGranulator::PAR_STACKTIMECURVE]),
          pitchRandomKnob(XapSlider::SS_Knob,
                          *p.granulator.idtoparmetadata[ToneGranulator::PAR_STACKRANDOMPITCH]),
          spatRandomKnob(
              XapSlider::SS_Knob,
              *p.granulator.idtoparmetadata[ToneGranulator::PAR_STACKRANDOMSPATIALIZATION])
    {
        initSlider(p, *this, countKnob);
        initSlider(p, *this, lengthKnob);
        initSlider(p, *this, warpKnob);
        initSlider(p, *this, pitchRandomKnob);
        initSlider(p, *this, spatRandomKnob);
    }
    void resized() override
    {
        juce::FlexBox flex;
        flex.flexDirection = juce::FlexBox::Direction::row;
        flex.items.add(juce::FlexItem(countKnob).withFlex(1.0).withMargin(2));
        flex.items.add(juce::FlexItem(lengthKnob).withFlex(1.0).withMargin(2));
        flex.items.add(juce::FlexItem(warpKnob).withFlex(1.0).withMargin(2));
        flex.items.add(juce::FlexItem(pitchRandomKnob).withFlex(1.0).withMargin(2));
        flex.items.add(juce::FlexItem(spatRandomKnob).withFlex(1.0).withMargin(2));
        flex.performLayout(juce::Rectangle<int>(7, 17, getWidth() - 14, getHeight() - 28));
    }
};

class PerformanceComponent : public juce::Component, public juce::Timer
{
  public:
    PerformanceComponent() { startTimer(100); }
    void timerCallback() override { repaint(); }
    void paint(juce::Graphics &g) override
    {
        g.fillAll(juce::Colours::black);
        int maxvoices = 0;
        int usedvoices = 0;
        float cpu_use = 0.0f;
        if (RequestData)
        {
            RequestData(maxvoices, usedvoices, cpu_use);
            float w = (float)(getWidth() - 2) / maxvoices * usedvoices * 0.5;
            g.setColour(juce::Colours::yellow);
            g.fillRect(juce::Rectangle<float>(0.0f, 0.0f, w, 20.0f));
            g.setColour(juce::Colours::white);
            g.drawText(fmt::format("VOICES {:2}/{:2}", usedvoices, maxvoices), 0, 0,
                       getWidth() / 2 - 2, 20, juce::Justification::centredRight);
            w = cpu_use * (getWidth() - 2) * 0.5;
            g.setColour(juce::Colours::green);
            g.fillRect(juce::Rectangle<float>(getWidth() / 2.0, 0.0f, w, 20.0f));
            g.setColour(juce::Colours::white);
            g.drawText(fmt::format("CPU {}%", (int)(cpu_use * 100.0)), getWidth() / 2 - 2, 0,
                       getWidth() / 2, 20, juce::Justification::centredRight);
        }
    }
    std::function<void(int &, int &, float &)> RequestData;
};

class MainOutputModule : public juce::GroupComponent
{
  public:
    AudioPluginAudioProcessor &processorRef;
    XapSlider mainVolumeKnob;
    XapSlider highPassCutoffKnob;
    PerformanceComponent perfComponent;
    MainOutputModule(AudioPluginAudioProcessor &p)
        : juce::GroupComponent("", "Main Output"), processorRef(p),
          mainVolumeKnob(XapSlider::SS_Knob,
                         *p.granulator.idtoparmetadata[ToneGranulator::PAR_MAINVOLUME]),
          highPassCutoffKnob(
              XapSlider::SS_Knob,
              *p.granulator.idtoparmetadata[ToneGranulator::PAR_MASTERHIGHPASSCUTOFF])
    {
        initSlider(p, *this, mainVolumeKnob);
        initSlider(p, *this, highPassCutoffKnob);
        addAndMakeVisible(perfComponent);
        addAndMakeVisible(highPassCutoffKnob);
        addAndMakeVisible(p.avisComponent);
    }
    void resized() override
    {
        mainVolumeKnob.setBounds(7, 17, 100, getHeight() - 25);
        highPassCutoffKnob.setBounds(mainVolumeKnob.getRight() + 2, 17, 100, getHeight() - 25);
        perfComponent.setBounds(highPassCutoffKnob.getRight() + 2, 17, getWidth() - 115, 21);
        processorRef.avisComponent.setBounds(highPassCutoffKnob.getRight() + 2,
                                             perfComponent.getBottom() + 1, getWidth() - 115,
                                             getHeight() - 25 - 21);
    }
};

class TimeModuleComponent : public juce::GroupComponent
{
  public:
    AudioPluginAudioProcessor &processorRef;
    XapSlider densityKnob;
    XapSlider durationKnob;
    XapSlider tailKnob;
    TimeModuleComponent(AudioPluginAudioProcessor &p)
        : juce::GroupComponent("", "Time"), processorRef(p),
          densityKnob(XapSlider::SS_Knob,
                      *p.granulator.idtoparmetadata[ToneGranulator::PAR_DENSITY]),
          durationKnob(XapSlider::SS_Knob,
                       *p.granulator.idtoparmetadata[ToneGranulator::PAR_DURATION]),
          tailKnob(XapSlider::SS_Knob, *p.granulator.idtoparmetadata[ToneGranulator::PAR_GRAINTAIL])
    {
        initSlider(p, *this, densityKnob);
        initSlider(p, *this, durationKnob);
        initSlider(p, *this, tailKnob);
    }
    void resized() override
    {
        juce::FlexBox flex;
        flex.flexDirection = juce::FlexBox::Direction::row;
        flex.items.add(juce::FlexItem(densityKnob).withFlex(1.0).withMargin(2));
        flex.items.add(juce::FlexItem(durationKnob).withFlex(1.0).withMargin(2));
        flex.items.add(juce::FlexItem(tailKnob).withFlex(1.0).withMargin(2));
        flex.performLayout(juce::Rectangle<int>(7, 17, getWidth() - 14, getHeight() - 28));
    }
};

class InsertModuleComponent : public juce::GroupComponent
{
  public:
    InsertModuleComponent(AudioPluginAudioProcessor &p, int insertIndex)
        : juce::GroupComponent("", fmt::format("Insert FX {}", char('A' + insertIndex))),
          processorRef(p), insertsIndex(insertIndex)
    {
        fillInsertDrop();
        addAndMakeVisible(insertDrop);
        insertDrop.OnItemSelected = [this]() { handleInsertSelection(); };
        int parstartindex = ToneGranulator::PAR_INSERTAFIRST + 32 * insertIndex;
        for (auto &pmd : p.granulator.parmetadatas)
        {
            if (pmd.id >= parstartindex && pmd.id < parstartindex + 32)
            {
                auto slid = std::make_unique<XapSlider>(XapSlider::SS_Knob, pmd);
                slid->OnValueChanged = [this, knobptr = slid.get()]() {
                    ParameterMessage msg;
                    msg.id = knobptr->getParameterMetaData().id;
                    msg.value = knobptr->getValue();
                    processorRef.params_from_gui_fifo.push(msg);
                };
                addAndMakeVisible(*slid);
                knobs.push_back(std::move(slid));
            }
        }
    }
    void fillInsertDrop()
    {
        insertDrop.rootNode.text = "FOO";
        std::map<std::string, DropDownComponent::Node *> nodemap;
        insertDrop.rootNode.children.reserve(32);
        auto inserttypes = GrainInsertFX::getAvailableModes();
        int filterID = 0;
        for (auto &mod : inserttypes)
        {
            if (!mod.groupname.empty() && !nodemap.contains(mod.groupname))
            {
                insertDrop.rootNode.children.push_back({mod.groupname, -1});
                nodemap[mod.groupname] = &insertDrop.rootNode.children.back();
            }
            if (!mod.groupname.empty())
            {
                nodemap[mod.groupname]->children.push_back({mod.displayname, filterID});
                filterInfoMap[filterID] = mod;
                ++filterID;
            }
            else
            {
                insertDrop.rootNode.children.push_back({mod.displayname, filterID});
                filterInfoMap[filterID] = mod;
                ++filterID;
            }
        }
        insertDrop.setSelectedId(0);
    }
    void handleInsertSelection()
    {
        auto it = filterInfoMap.find(insertDrop.getSelectedId());
        if (it != filterInfoMap.end())
        {
            DBG(it->second.displayname);
            ThreadMessage msg;
            msg.opcode = ThreadMessage::OP_FILTERTYPE;
            msg.filterindex = insertsIndex;
            msg.insertmainmode = it->second.mainmode;
            msg.awtype = it->second.awtype;
            msg.filtermodel = it->second.sstmodel;
            msg.filterconfig = it->second.sstconfig;
            processorRef.from_gui_fifo.push(msg);
        }
        juce::Timer::callAfterDelay(250, [this]() { updateInsertMetadatas(); });
    }
    void updateInsertMetadatas()
    {
        for (auto &s : knobs)
        {
            auto id = s->getParameterMetaData().id;
            auto pmd = processorRef.granulator.idtoparmetadata[id];
            if (s->getParameterMetaData().name != pmd->name)
            {
                s->setParameterMetaData(*pmd, false);
            }
        }
        if (OnInsertTypeChanged)
            OnInsertTypeChanged();
    }
    void resized() override
    {
        insertDrop.setBounds(7, 17, 275, 20);
        juce::FlexBox flex;
        flex.flexDirection = juce::FlexBox::Direction::row;
        for (auto &c : knobs)
        {
            flex.items.add(juce::FlexItem(*c).withFlex(1.0).withMargin(2));
        }
        flex.performLayout(juce::Rectangle<int>(7, 21 + 17, getWidth() - 14, getHeight() - 40));
    }
    AudioPluginAudioProcessor &processorRef;
    int insertsIndex = -1;
    std::function<void(void)> OnInsertTypeChanged;
    std::map<int64_t, GrainInsertFX::ModeInfo> filterInfoMap;
    DropDownComponent insertDrop;
    std::vector<std::unique_ptr<XapSlider>> knobs;
};

class VolumeModuleComponent : public juce::GroupComponent
{
  public:
    AudioPluginAudioProcessor &processorRef;
    XapSlider volumeKnob;
    XapSlider morphKnob;
    XapSlider startCurveSlider;
    XapSlider endCurveSlider;
    std::vector<std::unique_ptr<XapSlider>> pitchBandGainKnobs;
    VolumeEnvelopeComponent envcomp;
    VolumeModuleComponent(AudioPluginAudioProcessor &p)
        : juce::GroupComponent("", "Volume"), processorRef(p),
          volumeKnob(XapSlider::SS_Knob,
                     *p.granulator.idtoparmetadata[ToneGranulator::PAR_GRAINVOLUME]),
          morphKnob(XapSlider::SS_Knob,
                    *p.granulator.idtoparmetadata[ToneGranulator::PAR_ENVMORPH]),
          startCurveSlider(XapSlider::SS_HorizontalSlider,
                           *p.granulator.idtoparmetadata[ToneGranulator::PAR_VOLENVEASINGSTART]),
          endCurveSlider(XapSlider::SS_HorizontalSlider,
                         *p.granulator.idtoparmetadata[ToneGranulator::PAR_VOLENVEASINGEND]),
          envcomp(&p.granulator, false)
    {
        addAndMakeVisible(volumeKnob);
        volumeKnob.OnValueChanged = [this]() {
            onParamChanged(ToneGranulator::PAR_GRAINVOLUME, volumeKnob.getValue());
        };
        addAndMakeVisible(morphKnob);
        morphKnob.OnValueChanged = [this]() {
            onParamChanged(ToneGranulator::PAR_ENVMORPH, morphKnob.getValue());
        };
        initSlider(p, *this, startCurveSlider);
        initSlider(p, *this, endCurveSlider);
        addAndMakeVisible(envcomp);
        for (int i = 0; i < 7; ++i)
        {
            auto knob = std::make_unique<XapSlider>(
                XapSlider::SS_Knob,
                *p.granulator.idtoparmetadata[ToneGranulator::PAR_PITCHBANDGAIN0 + i * 10]);
            knob->OnValueChanged = [i, this, knobptr = knob.get()]() {
                onParamChanged(ToneGranulator::PAR_PITCHBANDGAIN0 + i * 10, knobptr->getValue());
            };
            addAndMakeVisible(*knob);
            pitchBandGainKnobs.push_back(std::move(knob));
        }
    }
    void onParamChanged(uint32_t id, float val)
    {
        ParameterMessage msg;
        msg.id = id;
        msg.value = val;
        processorRef.params_from_gui_fifo.push(msg);
    }
    void resized() override
    {
        juce::FlexBox flex;
        flex.flexDirection = juce::FlexBox::Direction::row;
        flex.items.add(juce::FlexItem(volumeKnob).withFlex(1.0).withMargin(2).withMinWidth(60));
        flex.items.add(juce::FlexItem(morphKnob).withFlex(1.0).withMargin(2).withMinWidth(60));
        flex.items.add(juce::FlexItem(startCurveSlider)
                           .withFlex(2.0)
                           .withMargin(2)
                           .withMaxHeight(25)
                           .withMinWidth(60));
        flex.items.add(juce::FlexItem(endCurveSlider)
                           .withFlex(2.0)
                           .withMargin(2)
                           .withMaxHeight(25)
                           .withMinWidth(60));
        flex.items.add(juce::FlexItem(envcomp).withFlex(1.0).withMargin(2));
        flex.performLayout(juce::Rectangle<int>(7, 17, getWidth() - 16, 70));
        juce::FlexBox pitchgainflex;
        pitchgainflex.flexDirection = juce::FlexBox::Direction::row;
        for (auto &c : pitchBandGainKnobs)
        {
            pitchgainflex.items.add(
                juce::FlexItem(*c).withFlex(1.0).withMargin(2).withMinWidth(60));
        }
        pitchgainflex.performLayout(juce::Rectangle<int>(7, 17 + 71, getWidth() - 16, 62));
    }
};

class SpatializationModuleComponent : public juce::GroupComponent
{
  public:
    SpatializationModuleComponent(AudioPluginAudioProcessor &p)
        : juce::GroupComponent("", "Spatialization"), processorRef(p),
          ambOrderDrop(XapSlider::SS_HorizontalSlider,
                       *p.granulator.idtoparmetadata[ToneGranulator::PAR_AMBORDER]),
          azimuthKnob(XapSlider::SS_Knob,
                      *p.granulator.idtoparmetadata[ToneGranulator::PAR_AZIMUTH]),
          elevationKnob(XapSlider::SS_Knob,
                        *p.granulator.idtoparmetadata[ToneGranulator::PAR_ELEVATION]),
          spreadKnob(XapSlider::SS_Knob,
                     *p.granulator.idtoparmetadata[ToneGranulator::PAR_AMBSPREAD]),
          rotateKnob(XapSlider::SS_Knob,
                     *p.granulator.idtoparmetadata[ToneGranulator::PAR_AMBROTATE])
    {
        addAndMakeVisible(ambOrderDrop);
        ambOrderDrop.OnValueChanged = [this]() {
            onParamChanged(ToneGranulator::PAR_AMBORDER, ambOrderDrop.getValue());
        };
        addAndMakeVisible(azimuthKnob);
        azimuthKnob.OnValueChanged = [this]() {
            onParamChanged(ToneGranulator::PAR_AZIMUTH, azimuthKnob.getValue());
        };
        addAndMakeVisible(elevationKnob);
        elevationKnob.OnValueChanged = [this]() {
            onParamChanged(ToneGranulator::PAR_ELEVATION, elevationKnob.getValue());
        };
        addAndMakeVisible(spreadKnob);
        spreadKnob.OnValueChanged = [this]() {
            onParamChanged(ToneGranulator::PAR_AMBSPREAD, spreadKnob.getValue());
        };
        addAndMakeVisible(rotateKnob);
        rotateKnob.OnValueChanged = [this]() {
            onParamChanged(ToneGranulator::PAR_AMBROTATE, rotateKnob.getValue());
        };
    }
    void onParamChanged(uint32_t id, float val)
    {
        ParameterMessage msg;
        msg.id = id;
        msg.value = val;
        processorRef.params_from_gui_fifo.push(msg);
    }
    void resized() override
    {
        ambOrderDrop.setBounds(7, 17, 300, 25);
        juce::FlexBox flex;
        flex.flexDirection = juce::FlexBox::Direction::row;
        flex.items.add(juce::FlexItem(azimuthKnob).withFlex(1.0).withMargin(2));
        flex.items.add(juce::FlexItem(elevationKnob).withFlex(1.0).withMargin(2));
        flex.items.add(juce::FlexItem(spreadKnob).withFlex(1.0).withMargin(2));
        flex.items.add(juce::FlexItem(rotateKnob).withFlex(1.0).withMargin(2));
        flex.performLayout(juce::Rectangle<int>(7, 17 + 26, getWidth() - 16, getHeight() - 48));
    }
    AudioPluginAudioProcessor &processorRef;
    XapSlider ambOrderDrop;
    XapSlider azimuthKnob;
    XapSlider elevationKnob;
    XapSlider spreadKnob;
    XapSlider rotateKnob;
};

class ParameterGroupComponent : public juce::GroupComponent
{
  public:
    bool m_horizlayout = false;
    ParameterGroupComponent(juce::String groupName, bool horizLayout)
        : juce::GroupComponent{"", groupName}
    {
        m_horizlayout = horizLayout;
    }
    void addSlider(std::unique_ptr<XapSlider> &&s)
    {
        addAndMakeVisible(s.get());
        sliders.push_back(std::move(s));
    }
    void addHeaderComponent(juce::Component *c)
    {
        addAndMakeVisible(c);
        headerComponents.push_back(c);
    }
    void resized() override
    {
        juce::FlexBox layout;
        layout.flexDirection = juce::FlexBox::Direction::column;
        float minh = 25.0;
        float maxh = 40.0f;
        float headercompflex = 1.0;
        if (m_horizlayout)
        {
            layout.flexDirection = juce::FlexBox::Direction::row;
            minh = 50.0;
            maxh = 80.0;
            headercompflex = 2.0;
        }

        layout.flexWrap = juce::FlexBox::Wrap::wrap;

        for (int i = 0; i < headerComponents.size(); ++i)
        {
            layout.items.add(juce::FlexItem(*headerComponents[i])
                                 .withFlex(headercompflex)
                                 .withMargin(2.0)
                                 .withMinHeight(minh)
                                 .withMinWidth(50)
                                 .withMaxWidth(getWidth()));
        }
        for (int i = 0; i < sliders.size(); ++i)
        {
            layout.items.add(juce::FlexItem(*sliders[i])
                                 .withFlex(1.0)
                                 .withMargin(2.0)
                                 .withMinHeight(20)
                                 .withMaxHeight(maxh)
                                 .withMinWidth(50)
                                 .withMaxWidth(getWidth()));
        }
        layout.performLayout(juce::Rectangle<int>(7, 17, getWidth() - 14, getHeight() - 25));
    }
    std::vector<juce::Component *> headerComponents;
    std::vector<std::unique_ptr<XapSlider>> sliders;
};
