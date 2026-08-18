#include "lfocomponent.h"

LFOComponent::LFOComponent(int index, ToneGranulator *g)
    : lfoindex(index), gr(g),
      rateSlider(XapSlider::SS_Knob, *g->idtoparmetadata[ToneGranulator::PAR_LFORATES + index]),
      deformSlider(XapSlider::SS_Knob, *g->idtoparmetadata[ToneGranulator::PAR_LFODEFORMS + index]),
      shiftSlider(XapSlider::SS_Knob, *g->idtoparmetadata[ToneGranulator::PAR_LFOSHIFTS + index]),
      warpSlider(XapSlider::SS_Knob, *g->idtoparmetadata[ToneGranulator::PAR_LFOWARPS + index]),
      shapeSlider(XapSlider::SS_HorizontalSlider,
                  *g->idtoparmetadata[ToneGranulator::PAR_LFOSHAPES + index]),
      unipolarSlider(XapSlider::SS_HorizontalSlider,
                     *g->idtoparmetadata[ToneGranulator::PAR_LFOUNIPOLARS + index]),
      masterSyncSlider(XapSlider::SS_HorizontalSlider,
                       *g->idtoparmetadata[ToneGranulator::PAR_LFOMASTERSYNCS + index])
{
    addAndMakeVisible(masterSyncSlider);
    masterSyncSlider.OnValueChanged = [this]() {
        stateChangedCallback(masterSyncSlider.getParameterMetaData().id,
                             masterSyncSlider.getValue());
    };

    addAndMakeVisible(rateSlider);
    rateSlider.OnValueChanged = [this]() {
        stateChangedCallback(rateSlider.getParameterMetaData().id, rateSlider.getValue());
    };

    addAndMakeVisible(deformSlider);
    deformSlider.OnValueChanged = [this]() {
        stateChangedCallback(deformSlider.getParameterMetaData().id, deformSlider.getValue());
    };

    addAndMakeVisible(shiftSlider);
    shiftSlider.OnValueChanged = [this]() {
        stateChangedCallback(shiftSlider.getParameterMetaData().id, shiftSlider.getValue());
    };

    addAndMakeVisible(warpSlider);
    warpSlider.OnValueChanged = [this]() {
        stateChangedCallback(warpSlider.getParameterMetaData().id, warpSlider.getValue());
    };

    addAndMakeVisible(shapeSlider);
    shapeSlider.OnValueChanged = [this]() {
        stateChangedCallback(shapeSlider.getParameterMetaData().id, shapeSlider.getValue());
    };

    addAndMakeVisible(unipolarSlider);
    unipolarSlider.OnValueChanged = [this]() {
        stateChangedCallback(unipolarSlider.getParameterMetaData().id, unipolarSlider.getValue());
    };
}

void LFOComponent::resized()
{
    shapeSlider.setBounds(0, 0, 200, 25);
    unipolarSlider.setBounds(shapeSlider.getRight() + 1, 0, 100, 25);
    masterSyncSlider.setBounds(unipolarSlider.getRight() + 1, 0, 100, 25);

    juce::FlexBox flex;
    flex.flexDirection = juce::FlexBox::Direction::row;

    flex.items.add(juce::FlexItem(rateSlider).withFlex(1.0).withMargin(2));
    flex.items.add(juce::FlexItem(deformSlider).withFlex(1.0).withMargin(2));
    flex.items.add(juce::FlexItem(shiftSlider).withFlex(1.0).withMargin(2));
    flex.items.add(juce::FlexItem(warpSlider).withFlex(1.0).withMargin(2));
    flex.performLayout(juce::Rectangle<int>(0, 25, getWidth(), getHeight() - 25));
}
