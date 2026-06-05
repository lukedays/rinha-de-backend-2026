import http from 'k6/http';
import { check } from 'k6';
import { SharedArray } from 'k6/data';

// payloads reais de exemplo; cada VU escolhe um aleatorio por iteracao
const payloads = new SharedArray('payloads', () => JSON.parse(open('/data/example-payloads.json')));

const VUS = parseInt(__ENV.VUS || '0');
const scenarios = VUS > 0
  ? { steady: { executor: 'constant-vus', vus: VUS, duration: __ENV.DUR || '20s' } }
  : {
      ramp: {
        executor: 'ramping-vus', startVUs: 1,
        stages: [
          { duration: '10s', target: 30 },
          { duration: '20s', target: 60 },
          { duration: '10s', target: 60 },
        ],
      },
    };

export const options = {
  scenarios,
  thresholds: {
    http_req_duration: ['p(99)<2001'],
    http_req_failed: ['rate<0.15'],
  },
};

const headers = { 'Content-Type': 'application/json' };
const target = (__ENV.TARGET || 'http://host.docker.internal:9999') + '/fraud-score';

export default function () {
  const p = payloads[Math.floor(Math.random() * payloads.length)];
  const res = http.post(target, JSON.stringify(p), { headers });
  check(res, {
    'status 200': (r) => r.status === 200,
    'has approved': (r) => r.body && r.body.indexOf('approved') >= 0,
  });
}
