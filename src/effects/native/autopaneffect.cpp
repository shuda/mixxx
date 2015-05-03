#include "util/math.h"
#include <QtDebug>

#include "effects/native/autopaneffect.h"

#include "sampleutil.h"
#include "util/experiment.h"

const float positionRampingThreshold = 0.005f;


// static
QString AutoPanEffect::getId() {
    return "org.mixxx.effects.autopan";
}

// static
EffectManifest AutoPanEffect::getManifest() {
    EffectManifest manifest;
    manifest.setId(getId());
    manifest.setName(QObject::tr("AutoPan"));
    manifest.setAuthor("The Mixxx Team");
    manifest.setVersion("1.0");
    manifest.setDescription(QObject::tr(
        "Bounce the sound from a channel to another, fastly or softly"));
    
    // Period unit
    EffectManifestParameter* periodUnit = manifest.addParameter();
    periodUnit->setId("periodUnit");
    periodUnit->setName(QObject::tr("Period Unit"));
    periodUnit->setDescription("Period Unit");
    periodUnit->setControlHint(EffectManifestParameter::CONTROL_TOGGLE_STEPPING);
    periodUnit->setSemanticHint(EffectManifestParameter::SEMANTIC_UNKNOWN);
    periodUnit->setUnitsHint(EffectManifestParameter::UNITS_UNKNOWN);
    periodUnit->setDefault(0);
    periodUnit->setMinimum(0);
    periodUnit->setMaximum(1);
    
    // Period
    EffectManifestParameter* period = manifest.addParameter();
    period->setId("period");
    period->setName(QObject::tr("Period"));
    period->setDescription("Controls the speed of the effect.");
    period->setControlHint(EffectManifestParameter::CONTROL_KNOB_LINEAR);
//    period->setControlHint(EffectManifestParameter::CONTROL_KNOB_LOGARITHMIC);
    period->setSemanticHint(EffectManifestParameter::SEMANTIC_UNKNOWN);
    period->setUnitsHint(EffectManifestParameter::UNITS_UNKNOWN);
    period->setMinimum(0.01);
    period->setMaximum(1.0);
    period->setDefault(1.0);
    
    /** /
    // Delay : applied on the channel with gain reducing.
    EffectManifestParameter* delay = manifest.addParameter();
    delay->setId("delay");
    delay->setName(QObject::tr("delay"));
    delay->setDescription("Controls length of the delay");
    delay->setControlHint(EffectManifestParameter::CONTROL_KNOB_LINEAR);
    delay->setSemanticHint(EffectManifestParameter::SEMANTIC_UNKNOWN);
    delay->setUnitsHint(EffectManifestParameter::UNITS_UNKNOWN);
    // 10 ms seams to be more then enough here for test purpose
    // it works for me up to ~5 ms using my laptop speakers.
    // for 40 ms you have an echo
    // http://en.wikipedia.org/wiki/Precedence_effect
    delay->setMinimum(-0.01);
    delay->setMaximum(0.01);
    delay->setDefault(0.0);
    /**/
    
    // This parameter controls the easing of the sound from a side to another.
    EffectManifestParameter* smoothing = manifest.addParameter();
    smoothing->setId("smoothing");
    smoothing->setName(QObject::tr("Smoothing"));
    smoothing->setDescription(
            QObject::tr("How fast the signal goes from a channel to an other"));
    smoothing->setControlHint(EffectManifestParameter::CONTROL_KNOB_LINEAR);
    smoothing->setSemanticHint(EffectManifestParameter::SEMANTIC_UNKNOWN);
    smoothing->setUnitsHint(EffectManifestParameter::UNITS_UNKNOWN);
    smoothing->setMinimum(0.0);
    smoothing->setMaximum(0.5);  // there are two steps per period so max is half
    smoothing->setDefault(0.0);
    
    // Width : applied on the channel with gain reducing.
    EffectManifestParameter* width = manifest.addParameter();
    width->setId("width");
    width->setName(QObject::tr("width"));
    width->setDescription("Controls length of the width");
    width->setControlHint(EffectManifestParameter::CONTROL_KNOB_LINEAR);
    width->setSemanticHint(EffectManifestParameter::SEMANTIC_UNKNOWN);
    width->setUnitsHint(EffectManifestParameter::UNITS_UNKNOWN);
    width->setMinimum(0.0);
    width->setMaximum(1.0);    // 0.02 * sampleRate => 20ms
    width->setDefault(0.00);
    
    return manifest;
}

AutoPanEffect::AutoPanEffect(EngineEffect* pEffect, const EffectManifest& manifest)
        : 
          m_pSmoothingParameter(pEffect->getParameterById("smoothing")),
          m_pPeriodUnitParameter(pEffect->getParameterById("periodUnit")),
          m_pPeriodParameter(pEffect->getParameterById("period")),
//          m_pDelayParameter(pEffect->getParameterById("delay")),
          m_pWidthParameter(pEffect->getParameterById("width"))
           {
    Q_UNUSED(manifest);
}

AutoPanEffect::~AutoPanEffect() {
}

void AutoPanEffect::processChannel(const ChannelHandle& handle, PanGroupState* pGroupState,
                              const CSAMPLE* pInput,
                              CSAMPLE* pOutput, const unsigned int numSamples,
                              const unsigned int sampleRate,
                              const EffectProcessor::EnableState enableState,
                              const GroupFeatureState& groupFeatures) {
    Q_UNUSED(handle);
    
    PanGroupState& gs = *pGroupState;
    
    if (enableState == EffectProcessor::DISABLED) {
        return;
    }
    
    double periodUnit = m_pPeriodUnitParameter->value();
    
    CSAMPLE width = m_pWidthParameter->value();
    CSAMPLE period = m_pPeriodParameter->value();
    if (periodUnit == 1 && groupFeatures.has_beat_length) {
        // 1/8, 1/4, 1/2, 1, 2, 4, 8, 16, 32, 64
        double beats = pow(2, floor(period * 9 / m_pPeriodParameter->maximum()) - 3);
        period = groupFeatures.beat_length * beats;
    } else {
        // max period is 50 seconds
        period *= sampleRate * 25.0;
    }
    
    CSAMPLE stepFrac = m_pSmoothingParameter->value();
    
    if (gs.time > period || enableState == EffectProcessor::ENABLING) {
        gs.time = 0;
    }
    
    // Normally, the position goes from 0 to 1 linearly. Here we make steps at
    // 0.25 and 0.75 to have the sound fully on the right or fully on the left.
    // At the end, the "position" value can describe a sinusoid or a square
    // curve depending of the size of those steps.
    
    // coef of the slope
    // a = (y2 - y1) / (x2 - x1)
    //       1  / ( 1 - 2 * stepfrac)
    float a = stepFrac != 0.5f ? 1.0f / (1.0f - stepFrac * 2.0f) : 1.0f;
    
    // size of a segment of slope (controled by the "strength" parameter)
    float u = (0.5f - stepFrac) / 2.0f;
    
    gs.frac.setRampingThreshold(positionRampingThreshold);
    gs.frac.ramped = false;     // just for debug
    
    double sinusoid = 0;
    
    for (unsigned int i = 0; i + 1 < numSamples; i += 2) {
        
        CSAMPLE periodFraction = CSAMPLE(gs.time) / period;
        
        // current quarter in the trigonometric circle
        float quarter = floorf(periodFraction * 4.0f);
        
        // part of the period fraction being a step (not in the slope)
        CSAMPLE stepsFractionPart = floorf((quarter+1.0f)/2.0f) * stepFrac;
        
        // float inInterval = fmod( periodFraction, (period / 2.0) );
        float inStepInterval = fmod(periodFraction, 0.5f);
        
        CSAMPLE angleFraction;
        if (inStepInterval > u && inStepInterval < (u + stepFrac)) {
            // at full left or full right
            angleFraction = quarter < 2.0f ? 0.25f : 0.75f;
        } else {
            // in the slope (linear function)
            angleFraction = (periodFraction - stepsFractionPart) * a;
        }
        
        // transforms the angleFraction into a sinusoid.
        // The width parameter modulates the two limits. if width values 0.5,
        // the limits will be 0.25 and 0.75. If it's 0, it will be 0.5 and 0.5
        // so the sound will be stuck at the center. If it values 1, the limits 
        // will be 0 and 1 (full left and full right).
        sinusoid = sin(M_PI * 2.0f * angleFraction) * width;
        gs.frac.setWithRampingApplied((sinusoid + 1.0f) / 2.0f);
        
        pOutput[i] = pInput[i] * gs.frac * 2;
        pOutput[i+1] = pInput[i+1] * (1.0f - gs.frac) * 2;
        
        gs.time++;
    }
    
    // apply the delay
//    float delay = round(m_pDelayParameter->value() * sampleRate);
//    gs.delay->setLeftDelay( delay * sinusoid );
    gs.delay->setLeftDelay( 0.01 * sinusoid );
    gs.delay->process(pOutput, pOutput, numSamples);
    
    qDebug()
        // << "| ramped :" << gs.frac.ramped
        << "| quarter :" << floorf(CSAMPLE(gs.time) / period * 4.0f)
        << "| delay :" << sinusoid / 10
        // << "| beat_length :" << groupFeatures.beat_length
        << "| beats :" << pow(2, floor(m_pPeriodParameter->value() * 9 / m_pPeriodParameter->maximum()) - 3)
        // << "| period :" << period
        << "| frac :" << gs.frac
        << "| time :" << gs.time
        << "| numSamples :" << numSamples
        ;
    /**/
}

