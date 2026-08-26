import WedgeModule from './wedge-node.js';

const M = await WedgeModule();
const c = (n, r, a) => M.cwrap(n, r, a);
const api = {
  init: c('wedge_init', null, ['number','number']),
  fb: c('wedge_framebuffer','number',[]),
  tick: c('wedge_tick', null, ['number','number']),
  event: c('wedge_event', null, ['number','number','number']),
  send: c('wedge_send', null, ['string','string','number','number','number']),
  state: c('wedge_state','number',[]),
  mode: c('wedge_mode','number',[]),
  bright: c('wedge_brightness','number',[]),
  pending: c('wedge_pending','number',[]),
  kept: c('wedge_has_kept','number',[]),
  card: c('wedge_card','number',[]),
  setClock: c('wedge_set_clock', null, ['number']),
};
const STATES=["BOOT","INITIALIZING","CONNECTING","SYNCING_TIME","HOME","MESSAGE_AVAILABLE","MESSAGE_PRESENTATION","DIAGNOSTIC"];
const MODES=["SLEEP","MORNING","DAY","EVENING","NIGHT"];
const BASE = 1786147200 + 7*3600;

const run = (n, dt=1/60) => { for (let i=0;i<n;i++) api.tick(dt, 0); };
const lum = () => {
  const p = api.fb(); const h = M.HEAPU8; let s=0;
  for (let i=0;i<536*240;i++) s += h[p+i*4]+h[p+i*4+1]+h[p+i*4+2];
  return s/(536*240*3);
};
let fails = 0;
const check = (name, cond, extra='') => { console.log(`${cond?'  ok  ':'  FAIL'} ${name}${extra?'  '+extra:''}`); if(!cond) fails++; };

// 21:42 night
api.init(BASE + (21*60+42)*60, 0);
run(10);
check('boots into a boot state', api.state() <= 3, STATES[api.state()]);
api.event(7,0,0); api.event(9,0,0);          // wifi up, time synced
run(120);
check('settles to HOME', STATES[api.state()]==='HOME', STATES[api.state()]);
check('mode is NIGHT at 21:42', MODES[api.mode()]==='NIGHT', MODES[api.mode()]);
const nightLum = lum();
check('panel renders something', nightLum > 1 && nightLum < 120, 'mean luma '+nightLum.toFixed(1));
const nightBright = api.bright();
check('night emission is low', nightBright > 5 && nightBright < 90, nightBright+'/255');

// day brightness must be much higher
api.setClock(BASE + 13*3600); run(400);
check('mode is DAY at 13:00', MODES[api.mode()]==='DAY', MODES[api.mode()]);
check('day emission exceeds night', api.bright() > nightBright*2, api.bright()+' vs '+nightBright);
check('day panel is brighter', lum() > nightLum, lum().toFixed(1));

// message lifecycle
api.setClock(BASE + (21*60+42)*60); run(200);
check('no message waiting yet', api.pending()===0);
api.send('t1', "You've got this. I know today is the big one.", 3, 1, 0);
run(60);
check('message is waiting', api.pending()===1, 'pending='+api.pending());
check('state is MESSAGE_AVAILABLE', STATES[api.state()]==='MESSAGE_AVAILABLE', STATES[api.state()]);

// press feedback then open
api.event(1, 452, 132); run(6);
api.event(3, 452, 132); run(4);
check('card starts rising on release', api.card() > 0.02, 'card='+api.card().toFixed(3));
run(90);
check('card fully open', api.card() > 0.95, 'card='+api.card().toFixed(3));
check('state is PRESENTATION', STATES[api.state()]==='MESSAGE_PRESENTATION', STATES[api.state()]);
const openLum = lum();
check('open card changes the picture', Math.abs(openLum-nightLum) > 1, 'luma '+openLum.toFixed(1));

// drag to dismiss
api.event(1, 268, 60); run(2);
for (let i=1;i<=8;i++){ api.event(2, 268, 60+i*14); run(2); }
api.event(3, 268, 180); run(120);
check('card dismissed', api.card() < 0.05, 'card='+api.card().toFixed(3));
check('back to HOME-ish', STATES[api.state()]==='HOME'||STATES[api.state()]==='MESSAGE_AVAILABLE', STATES[api.state()]);
check('message no longer waiting', api.pending()===0, 'pending='+api.pending());
check('message is KEPT', api.kept()===1, 'kept='+api.kept());

// kept message re-opens
api.event(1, 452, 132); run(4); api.event(3, 452, 132); run(90);
check('kept message re-opens', api.card() > 0.9, 'card='+api.card().toFixed(3));

// diagnostics
api.event(6,0,0); run(20);
check('long press opens diagnostics', STATES[api.state()]==='DIAGNOSTIC', STATES[api.state()]);

console.log(fails ? `\n${fails} FAILED` : '\nall checks passed');
process.exit(fails?1:0);
