import http from 'k6/http';

export const options = {
  scenarios: { max: { executor: 'constant-vus', vus: parseInt(__ENV.VUS || '50'), duration: '12s' } },
};

const target = (__ENV.TARGET || 'http://nginx:9999') + '/ready';

export default function () {
  http.get(target);
}
