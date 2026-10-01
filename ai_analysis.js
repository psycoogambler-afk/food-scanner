/* ============================================================================
 *  SMART FOOD FRESHNESS SCANNER — AI analysis layer
 *  Heuristic freshness engine. Runs in the browser (window.FoodAI) and in Node.
 *
 *  DISCLAIMER: experimental estimate from low-cost sensors. NOT a scientific
 *  food-safety certification.
 * ==========================================================================*/
(function (global) {
  'use strict';

  var CONFIG = {
    gasBaselinePPM: 400,     // clean-air baseline for MQ-135
    gasSpoilPPM: 2500,       // level treated as strong spoilage indicator
    tempIdealMin: 2,         // cold-chain storage
    tempIdealMax: 10,
    tempHardMax: 28,         // tropical ambient ceiling
    humIdealMin: 40,
    humIdealMax: 70,
    weights: { gas: 45, temp: 25, hum: 15, color: 15 }
  };

  function clamp(v, lo, hi) { return Math.min(hi, Math.max(lo, v)); }

  /* Smoothed analysis over the current reading (single-shot).
     Pass a history array as 4th arg to smooth across readings. */
  function analyze(temp, hum, gas, history) {
    var t = Number(temp) || 0, h = Number(hum) || 0, g = Number(gas) || 0;

    // --- penalties ------------------------------------------------------------
    var gasRatio = clamp((g - CONFIG.gasBaselinePPM) /
                         (CONFIG.gasSpoilPPM - CONFIG.gasBaselinePPM), 0, 1);
    var gasPenalty = gasRatio * CONFIG.weights.gas;

    var tempPenalty = 0;
    if (t < CONFIG.tempIdealMin)      tempPenalty = (CONFIG.tempIdealMin - t) * 2.0;
    else if (t > CONFIG.tempIdealMax) tempPenalty = (t - CONFIG.tempIdealMax) * 2.0;
    tempPenalty = clamp(tempPenalty, 0, CONFIG.weights.temp);

    var humPenalty = 0;
    if (h < CONFIG.humIdealMin)      humPenalty = (CONFIG.humIdealMin - h) * 0.6;
    else if (h > CONFIG.humIdealMax) humPenalty = (h - CONFIG.humIdealMax) * 0.6;
    humPenalty = clamp(humPenalty, 0, CONFIG.weights.hum);

    // --- score -----------------------------------------------------------------
    var score = Math.round(clamp(100 - gasPenalty - tempPenalty - humPenalty, 0, 100));

    // optional smoothing across history of scores
    if (history && history.length) {
      var avg = history.reduce(function (a, b) { return a + b; }, score) / (history.length + 1);
      score = Math.round(avg);
    }

    var verdict = score >= 75 ? 'FRESH' : score >= 45 ? 'CHECK FOOD' : 'POSSIBLE SPOILAGE';

    // --- confidence + tips ------------------------------------------------------
    var confidence = Math.round(clamp(55 + gasRatio * 30 +
      (gasPenalty + tempPenalty + humPenalty > 0 ? 10 : 0), 50, 97));

    var tips = [];
    if (verdict === 'FRESH') {
      tips.push('Sensor values are within expected fresh-food ranges.');
      if (t > 10) tips.push('Store refrigerated (2–10 °C) to maintain freshness.');
    }
    if (gasRatio > 0.15) tips.push('Elevated VOC/gas level — spoilage gases (amines/sulfur compounds) may be present.');
    if (t > CONFIG.tempIdealMax) tips.push('Temperature above ideal storage range — bacterial growth risk increases.');
    if (t < CONFIG.tempIdealMin) tips.push('Temperature near/below freezing — texture may be affected.');
    if (h > CONFIG.humIdealMax) tips.push('High humidity — accelerates mold and spoilage.');
    if (h < CONFIG.humIdealMin) tips.push('Very dry air — food may dehydrate and degrade faster.');
    if (verdict === 'POSSIBLE SPOILAGE') {
      tips.push('Strong spoilage indicators detected — inspect smell/appearance and discard if in doubt.');
    } else if (verdict === 'CHECK FOOD') {
      tips.push('One or more readings trending out of range — inspect the food manually before use.');
    }
    tips.push('Estimate only — verify with sight, smell and date labels.');

    return { score: score, verdict: verdict, confidence: confidence,
             penalties: { gas: Math.round(gasPenalty), temp: Math.round(tempPenalty), hum: Math.round(humPenalty) },
             tips: tips };
  }

  var FoodAI = { analyze: analyze, config: CONFIG };

  if (typeof module !== 'undefined' && module.exports) module.exports = FoodAI;
  else global.FoodAI = FoodAI;

})(typeof window !== 'undefined' ? window : globalThis);

/* ---- standalone demo (node ai/ai_analysis.js) ---- */
if (typeof require !== 'undefined' && require.main === module) {
  var samples = [
    { name: 'fresh milk in fridge  ', temp: 4,  hum: 55, gas: 420  },
    { name: 'room-temp leftovers   ', temp: 24, hum: 65, gas: 1150 },
    { name: 'old fish (spoilage)   ', temp: 26, hum: 82, gas: 2300 }
  ];
  console.log('Smart Food Freshness Scanner — AI self-test\n');
  samples.forEach(function (s) {
    var r = require('./ai_analysis.js').analyze(s.temp, s.hum, s.gas);
    var icon = r.verdict === 'FRESH' ? '🟢' : r.verdict === 'CHECK FOOD' ? '🟡' : '🔴';
    console.log(icon + ' ' + s.name + ' -> ' + r.verdict + ' (' + r.score + '%) ' +
                'conf:' + r.confidence + '%  penalties:' + JSON.stringify(r.penalties));
  });
}
