#!/usr/bin/env python3
"""
Smart Food Freshness Scanner - optional AI micro-service.
Receives sensor JSON (and optionally an image) from the ESP32/dashboard,
runs the heuristic freshness engine, and (optionally) asks an LLM to write
a friendly explanation when OPENAI_API_KEY is set.

Run:  pip install flask flask-cors && python ai_server.py   (port 5000)
"""
import os
import json
import math

from dotenv import load_dotenv
load_dotenv()

from flask import Flask, request, jsonify
from flask_cors import CORS

app = Flask(__name__)
CORS(app)

# ---------------- heuristic engine (mirrors ai/ai_analysis.js) ----------------
CFG = dict(gas_base=400.0, gas_spoil=2500.0,
           t_min=2.0, t_max=10.0, h_min=40.0, h_max=70.0,
           w=dict(gas=45.0, temp=25.0, hum=15.0))

def clamp(v, lo, hi): return max(lo, min(hi, v))

def analyze(temp, hum, gas):
    t, h, g = float(temp), float(hum), float(gas)
    gas_ratio = clamp((g - CFG['gas_base']) / (CFG['gas_spoil'] - CFG['gas_base']), 0, 1)
    gas_pen = gas_ratio * CFG['w']['gas']
    if t < CFG['t_min']:      t_pen = (CFG['t_min'] - t) * 2.0
    elif t > CFG['t_max']:    t_pen = (t - CFG['t_max']) * 2.0
    else:                     t_pen = 0.0
    t_pen = clamp(t_pen, 0, CFG['w']['temp'])
    if h < CFG['h_min']:      h_pen = (CFG['h_min'] - h) * 0.6
    elif h > CFG['h_max']:    h_pen = (h - CFG['h_max']) * 0.6
    else:                     h_pen = 0.0
    h_pen = clamp(h_pen, 0, CFG['w']['hum'])

    score = round(clamp(100 - gas_pen - t_pen - h_pen, 0, 100))
    verdict = 'FRESH' if score >= 75 else 'CHECK FOOD' if score >= 45 else 'POSSIBLE SPOILAGE'
    return dict(score=score, verdict=verdict,
                confidence=round(clamp(55 + gas_ratio * 30, 50, 97)),
                penalties=dict(gas=round(gas_pen), temp=round(t_pen), hum=round(h_pen)))

def default_explanation(res):
    v = res['verdict']
    if v == 'FRESH':
        return 'All sensor readings are within expected fresh-food ranges. Keep refrigerated.'
    if v == 'CHECK FOOD':
        return 'Some readings are out of the ideal range. Inspect the food manually before use.'
    return ('Strong spoilage indicators (elevated VOC gases and/or adverse temperature '
            'and humidity). Inspect smell and appearance; discard if in doubt.')

# ---------------- optional LLM explanation ----------------
def llm_explain(payload, res):
    key = os.environ.get('OPENAI_API_KEY')
    if not key:
        return None
    try:
        import urllib.request
        prompt = (
            'You are a food-monitoring assistant. In 2 short sentences, explain to a home user '
            'what this sensor-based freshness estimate means and what to do. Be careful: this is '
            'an estimate, not a safety certification.\n\n'
            f'Data: {json.dumps(payload)}\nResult: {json.dumps(res)}'
        )
        req = urllib.request.Request(
            'https://api.openai.com/v1/chat/completions',
            data=json.dumps({
                'model': os.environ.get('OPENAI_MODEL', 'gpt-4o-mini'),
                'messages': [{'role': 'user', 'content': prompt}],
                'max_tokens': 120}).encode(),
            headers={'Content-Type': 'application/json', 'Authorization': 'Bearer ' + key})
        with urllib.request.urlopen(req, timeout=20) as r:
            data = json.loads(r.read())
        return data['choices'][0]['message']['content'].strip()
    except Exception:
        return None

# ---------------- endpoints ----------------
@app.get('/health')
def health():
    return jsonify(status='ok', ai='ready',
                   llm='enabled' if os.environ.get('OPENAI_API_KEY') else 'heuristic-only')

@app.post('/analyze')
def analyze_ep():
    payload = request.get_json(force=True, silent=True) or {}
    # image may be attached (ESP32-CAM / phone) for future vision models
    result = analyze(payload.get('temp', 25), payload.get('hum', 50), payload.get('gas', 400))
    explanation = llm_explain(payload, result) or default_explanation(result)
    result['explanation'] = explanation
    return jsonify(result)

if __name__ == '__main__':
    port = int(os.environ.get('PORT', 5000))
    print(f'AI server on http://0.0.0.0:{port}  (POST /analyze, GET /health)')
    app.run(host='0.0.0.0', port=port)
