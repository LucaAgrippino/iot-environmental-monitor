/* P · Schneider/Siemens operator grade — all 13 screens, 800×480 */
const PT = {
  bg:'#0D1117', surf:'#161B22', surf2:'#1B2129', border:'#2A2E34',
  ink:'#E8F2FA', muted:'#A0A8B0', dim:'#6B7280',
  ok:'#6FBF8E', warn:'#D4A84C', err:'#CC6666', accent:'#7BAFD4',
  sans:"'Inter', system-ui, sans-serif",
  mono:"'JetBrains Mono', ui-monospace, monospace",
};

/* ── icon set (line, inherit stroke) ── */
const PIcon = ({name, size=18}) => {
  const s={width:size, height:size, stroke:'currentColor', strokeWidth:1.6, fill:'none', strokeLinecap:'round', strokeLinejoin:'round'};
  const p={
    sensors:<g><circle cx="12" cy="12" r="3"/><path d="M12 2v3M12 19v3M2 12h3M19 12h3M5.6 5.6l2.1 2.1M16.3 16.3l2.1 2.1M5.6 18.4l2.1-2.1M16.3 7.7l2.1-2.1"/></g>,
    status:<path d="M3 12h4l3-9 4 18 3-9h4"/>,
    alarms:<g><path d="M12 2L1.5 22h21z"/><path d="M12 9v6M12 18.5h0"/></g>,
    config:<g><circle cx="12" cy="12" r="3"/><path d="M19.4 12c0 .4-.1.8-.1 1.2l2 1.5-2 3.5-2.4-.9c-.6.5-1.4.9-2.1 1.2l-.4 2.5h-4l-.4-2.5c-.7-.3-1.5-.7-2.1-1.2l-2.4.9-2-3.5 2-1.5c-.1-.4-.1-.8-.1-1.2s0-.8.1-1.2l-2-1.5 2-3.5 2.4.9c.6-.5 1.4-.9 2.1-1.2l.4-2.5h4l.4 2.5c.7.3 1.5.7 2.1 1.2l2.4-.9 2 3.5-2 1.5c.1.4.1.8.1 1.2z"/></g>,
    check:<path d="M5 12l5 5L20 7"/>,
    x:<path d="M6 6l12 12M18 6L6 18"/>,
    bolt:<path d="M13 2L3 14h7l-1 8 10-12h-7l1-8z"/>,
    shield:<path d="M12 2L3 6v6c0 5 4 9 9 10 5-1 9-5 9-10V6l-9-4z"/>,
  }[name];
  return <svg viewBox="0 0 24 24" style={s}>{p}</svg>;
};

/* ── frame chrome (header + tab bar) ── */
const PFrame = ({title, time='12:34:56', state='running', activeTab=0, children, noTabs=false, label}) => {
  const stateMap = {
    running:{c:PT.ok, t:'● RUNNING'},
    init:{c:PT.accent, t:'● INIT'},
    alarm:{c:PT.err, t:'■ ALARM'},
    update:{c:PT.accent, t:'⇪ UPDATE'},
  };
  const st = stateMap[state] || stateMap.running;
  const tabs = ['SENSORS','STATUS','ALARMS','CONFIG'];
  const tabIcons = ['sensors','status','alarms','config'];
  return (
    <div style={{width:800, height:480, background:PT.bg, color:PT.ink, fontFamily:PT.sans, position:'relative', overflow:'hidden', border:'1px solid #000'}}>
      {!noTabs && (
        <div style={{height:64, display:'grid', gridTemplateColumns:'repeat(4,1fr)', background:'#0A0E14', borderBottom:`1px solid ${PT.border}`}}>
          {tabs.map((t,i)=>(
            <div key={t} style={{display:'flex', flexDirection:'column', alignItems:'center', justifyContent:'center', gap:4, fontSize:11, fontWeight:600, letterSpacing:'0.14em', color:i===activeTab?PT.ink:'#888', borderRight:i<3?`1px solid ${PT.border}`:'none', position:'relative', background:i===activeTab?'rgba(123,175,212,0.10)':'transparent'}}>
              {i===activeTab && <div style={{position:'absolute', top:0, left:0, right:0, height:3, background:PT.accent}}></div>}
              <span style={{color:i===activeTab?PT.accent:'#888'}}><PIcon name={tabIcons[i]} size={18}/></span>
              {t}
            </div>
          ))}
        </div>
      )}
      {!noTabs && (
        <div style={{height:40, padding:'0 22px', display:'flex', alignItems:'center', justifyContent:'space-between', borderBottom:`1px solid ${PT.border}`}}>
          <span style={{fontSize:11, fontWeight:700, letterSpacing:'0.18em', textTransform:'uppercase'}}>{title}</span>
          <div style={{display:'flex', alignItems:'center', gap:14, fontFamily:PT.mono, fontSize:12, color:PT.muted}}>
            <span>{time}</span>
            <span style={{padding:'3px 10px', fontFamily:PT.mono, fontSize:10, fontWeight:700, letterSpacing:'0.16em', color:st.c, border:`1px solid ${st.c}`, background:`${st.c}14`}}>{st.t}</span>
          </div>
        </div>
      )}
      <div style={{position:'absolute', top:noTabs?0:104, bottom:0, left:0, right:0}}>
        {children}
      </div>
    </div>
  );
};

/* ── sensor card primitive ── */
const PSensorCard = ({label, value, unit, status='valid', time='12:34:56', delta='+0.3 / 5m', sparkColor=PT.accent, dashed=false, faded=false, alarm=false}) => {
  const stripe = status==='valid'?PT.ok : status==='stale'?PT.warn : status==='error'?PT.err : status==='wait'?PT.dim : PT.ok;
  const valColor = faded ? PT.dim : (status==='stale'?PT.warn : status==='error'?PT.err : alarm?PT.err : PT.ink);
  const statusText = {valid:'● VALID', stale:'⚠ STALE', error:'✕ ERROR', wait:'… WAITING', alarm:'■ ALARM'}[status] || '● VALID';
  const statusColor = {valid:PT.ok, stale:PT.warn, error:PT.err, wait:PT.muted, alarm:PT.err}[status];
  return (
    <div style={{background:PT.surf, border:`1px solid ${PT.border}`, position:'relative', padding:'16px 18px', overflow:'hidden'}}>
      <div style={{position:'absolute', left:0, top:0, bottom:0, width:4, background:stripe}}></div>
      <div style={{fontSize:10, fontWeight:700, letterSpacing:'0.22em', color:PT.muted, textTransform:'uppercase', marginBottom:14}}>{label}</div>
      <div style={{display:'flex', alignItems:'baseline', gap:8, minHeight:54}}>
        {value!==null ? <>
          <span style={{fontFamily:PT.mono, fontSize:54, fontWeight:600, color:valColor, letterSpacing:'-0.03em', lineHeight:1}}>{value}</span>
          <span style={{fontFamily:PT.mono, fontSize:14, color:PT.muted}}>{unit}</span>
        </> : <span style={{fontFamily:PT.mono, fontSize:18, color:PT.muted}}>Waiting for data…</span>}
      </div>
      {value!==null && <svg viewBox="0 0 200 34" preserveAspectRatio="none" style={{marginTop:14, width:'100%', height:34, display:'block'}}>
        {!dashed && <path d="M0 22 L20 20 L40 24 L60 18 L80 21 L100 16 L120 14 L140 18 L160 12 L180 14 L200 10 L200 34 L0 34 Z" fill={sparkColor} opacity="0.12"/>}
        <path d="M0 22 L20 20 L40 24 L60 18 L80 21 L100 16 L120 14 L140 18 L160 12 L180 14 L200 10" stroke={faded?PT.dim:sparkColor} strokeWidth="1.5" fill="none" strokeDasharray={dashed?'3 3':null}/>
      </svg>}
      <div style={{display:'flex', justifyContent:'space-between', marginTop:14, fontFamily:PT.mono, fontSize:11, color:PT.muted}}>
        <span style={{color:statusColor}}>{statusText} · {time}</span>
        <span>{value!==null ? delta : '—'}</span>
      </div>
    </div>
  );
};

/* ── 1. SPLASH (boot) ── */
const PSplash = () => (
  <div style={{width:800, height:480, background:PT.bg, color:PT.ink, fontFamily:PT.sans, position:'relative', overflow:'hidden', border:'1px solid #000', display:'flex', flexDirection:'column', alignItems:'center', justifyContent:'center'}}>
    <div style={{display:'flex', alignItems:'center', gap:14, marginBottom:30}}>
      <svg viewBox="0 0 64 64" width="56" height="56" fill="none" stroke={PT.accent} strokeWidth="2.4" strokeLinecap="round" strokeLinejoin="round">
        <rect x="4" y="14" width="56" height="36" rx="3"/>
        <path d="M4 26 L60 26"/><path d="M14 14 L14 6 L50 6 L50 14"/>
        <circle cx="13" cy="20" r="1.6" fill={PT.accent}/>
        <path d="M22 38 L28 38 L31 32 L37 44 L40 38 L48 38" stroke={PT.ok}/>
      </svg>
      <div>
        <div style={{fontSize:30, fontWeight:700, letterSpacing:'-0.02em'}}>ENVMON</div>
        <div style={{fontFamily:PT.mono, fontSize:11, letterSpacing:'0.22em', color:PT.muted, fontWeight:600, textTransform:'uppercase', marginTop:2}}>Environmental Gateway</div>
      </div>
    </div>
    <div style={{width:420, marginTop:20}}>
      <div style={{height:4, background:PT.surf, border:`1px solid ${PT.border}`, position:'relative', overflow:'hidden'}}>
        <div style={{position:'absolute', left:0, top:0, bottom:0, width:'62%', background:`linear-gradient(90deg, ${PT.accent}, ${PT.accent}dd)`}}></div>
      </div>
      <div style={{display:'flex', justifyContent:'space-between', marginTop:10, fontFamily:PT.mono, fontSize:11, color:PT.muted}}>
        <span style={{color:PT.ink}}>Initializing sensor bus…</span>
        <span>62%</span>
      </div>
    </div>
    <div style={{position:'absolute', bottom:24, left:24, right:24, display:'flex', justifyContent:'space-between', fontFamily:PT.mono, fontSize:10, color:PT.muted, letterSpacing:'0.14em', textTransform:'uppercase'}}>
      <span>FW v2.4.1 · build 20260426 · LVGL 9.2</span>
      <span>STM32F469 · 800×480</span>
    </div>
    <div style={{position:'absolute', top:24, right:24, display:'flex', alignItems:'center', gap:8, fontFamily:PT.mono, fontSize:11, color:PT.muted}}>
      <span style={{width:8, height:8, borderRadius:'50%', background:PT.accent, boxShadow:`0 0 8px ${PT.accent}`}}></span>BOOT
    </div>
  </div>
);

/* ── 2. FIRMWARE UPDATE ── */
const PFirmware = () => (
  <div style={{width:800, height:480, background:PT.bg, color:PT.ink, fontFamily:PT.sans, position:'relative', overflow:'hidden', border:'1px solid #000'}}>
    <div style={{height:40, padding:'0 22px', display:'flex', alignItems:'center', justifyContent:'space-between', borderBottom:`1px solid ${PT.border}`}}>
      <span style={{fontSize:11, fontWeight:700, letterSpacing:'0.18em', textTransform:'uppercase'}}>Firmware Update</span>
      <div style={{display:'flex', alignItems:'center', gap:14, fontFamily:PT.mono, fontSize:12, color:PT.muted}}>
        <span>12:34:56</span>
        <span style={{padding:'3px 10px', fontSize:10, fontWeight:700, letterSpacing:'0.16em', color:PT.accent, border:`1px solid ${PT.accent}`, background:`${PT.accent}14`}}>⇪ UPDATING</span>
      </div>
    </div>
    <div style={{padding:36, display:'flex', flexDirection:'column', alignItems:'center'}}>
      <div style={{display:'flex', alignItems:'center', gap:18, marginBottom:30}}>
        <span style={{color:PT.accent}}><PIcon name="bolt" size={32}/></span>
        <div>
          <div style={{fontSize:22, fontWeight:600, letterSpacing:'-0.01em'}}>Updating firmware</div>
          <div style={{fontFamily:PT.mono, fontSize:12, color:PT.muted, marginTop:2}}>Do not power off the device</div>
        </div>
      </div>
      <div style={{width:'100%', maxWidth:540}}>
        <div style={{display:'flex', justifyContent:'space-between', fontFamily:PT.mono, fontSize:11, color:PT.muted, marginBottom:8, letterSpacing:'0.06em'}}>
          <span style={{color:PT.ink}}>Writing partition · stage 3 of 5</span>
          <span>47% · 4.2 MB / 9.0 MB</span>
        </div>
        <div style={{height:14, background:PT.surf, border:`1px solid ${PT.border}`, position:'relative', overflow:'hidden'}}>
          <div style={{position:'absolute', left:0, top:0, bottom:0, width:'47%', background:`linear-gradient(90deg, ${PT.accent}, #5C9FC7)`}}></div>
          <div style={{position:'absolute', inset:0, background:'repeating-linear-gradient(45deg, transparent 0 6px, rgba(255,255,255,0.04) 6px 12px)', mixBlendMode:'overlay'}}></div>
        </div>
        <div style={{display:'grid', gridTemplateColumns:'1fr 1fr', gap:14, marginTop:24}}>
          <div style={{background:PT.surf, border:`1px solid ${PT.border}`, padding:'12px 16px'}}>
            <div style={{fontSize:10, fontWeight:700, letterSpacing:'0.22em', color:PT.muted, textTransform:'uppercase', marginBottom:6}}>Current</div>
            <div style={{fontFamily:PT.mono, fontSize:18, color:PT.ink}}>v2.4.1</div>
            <div style={{fontFamily:PT.mono, fontSize:11, color:PT.muted, marginTop:2}}>build 20260426</div>
          </div>
          <div style={{background:PT.surf, border:`1px solid ${PT.accent}`, padding:'12px 16px', position:'relative'}}>
            <div style={{position:'absolute', left:0, top:0, bottom:0, width:3, background:PT.accent}}></div>
            <div style={{fontSize:10, fontWeight:700, letterSpacing:'0.22em', color:PT.accent, textTransform:'uppercase', marginBottom:6}}>Installing</div>
            <div style={{fontFamily:PT.mono, fontSize:18, color:PT.ink}}>v2.5.0</div>
            <div style={{fontFamily:PT.mono, fontSize:11, color:PT.muted, marginTop:2}}>build 20260509</div>
          </div>
        </div>
        <div style={{marginTop:24, fontFamily:PT.mono, fontSize:11, color:PT.muted, lineHeight:1.7}}>
          <div><span style={{color:PT.ok}}>✓</span> Verified signature · SHA-256 match</div>
          <div><span style={{color:PT.ok}}>✓</span> Erased target partition · 9.0 MB</div>
          <div><span style={{color:PT.accent}}>⟳</span> Writing image · ~2 min remaining</div>
          <div style={{color:PT.dim}}><span>○</span> Verify checksum</div>
          <div style={{color:PT.dim}}><span>○</span> Reboot &amp; commit</div>
        </div>
      </div>
    </div>
  </div>
);

/* ── 3-6. SENSORS variants ── */
const PSensors = ({variant='normal'}) => {
  const v = variant;
  const tHum = <PSensorCard label="Humidity · S2" value="58.2" unit="%RH" status="valid" delta="−0.4 / 5m" sparkColor={PT.accent}/>;
  let cards;
  if (v==='waiting') cards = (<>
    <PSensorCard label="Temperature · S1" value={null} unit="°C" status="wait"/>
    <PSensorCard label="Humidity · S2" value={null} unit="%RH" status="wait"/>
    <PSensorCard label="Pressure · S3" value={null} unit="hPa" status="wait"/>
  </>);
  else if (v==='error') cards = (<>
    <PSensorCard label="Temperature · S1" value="23.4" unit="°C" status="error" delta="last 12:30:02" faded sparkColor={PT.err}/>
    {tHum}
    <PSensorCard label="Pressure · S3" value="1013" unit="hPa" status="valid" delta="+0.1 / 5m"/>
  </>);
  else if (v==='stale') cards = (<>
    <PSensorCard label="Temperature · S1" value="23.4" unit="°C" status="valid" delta="+0.3 / 5m"/>
    {tHum}
    <PSensorCard label="Pressure · S3" value="1013" unit="hPa" status="stale" time="12:31:14" delta="—" dashed sparkColor={PT.warn}/>
  </>);
  else cards = (<>
    <PSensorCard label="Temperature · S1" value="23.4" unit="°C" status="valid" delta="+0.3 / 5m"/>
    {tHum}
    <PSensorCard label="Pressure · S3" value="1013" unit="hPa" status="valid" delta="+0.1 / 5m"/>
  </>);
  return (
    <PFrame title="Sensor Readings" activeTab={0} state={v==='waiting'?'init':'running'}>
      <div style={{padding:22, display:'grid', gridTemplateColumns:'1fr 1fr 1fr', gap:14}}>{cards}</div>
      <div style={{position:'absolute', bottom:0, left:0, right:0, padding:'10px 22px', borderTop:`1px solid ${PT.border}`, background:'#0A0E14', display:'flex', justifyContent:'space-between', fontFamily:PT.mono, fontSize:11, color:PT.muted}}>
        <span>POLL 5s · NEXT {v==='waiting'?'5s':'3s'}</span>
        <span>{v==='waiting'?'Acquiring…':'STM32F469 · LVGL · 800×480'}</span>
      </div>
    </PFrame>
  );
};

/* ── 7-8. STATUS ── */
const PStatusRow = ({label, value, status='ok'}) => {
  const c = status==='ok'?PT.ok : status==='warn'?PT.warn : status==='err'?PT.err : PT.muted;
  return (
    <div style={{display:'flex', alignItems:'center', justifyContent:'space-between', padding:'9px 0', borderBottom:`1px solid ${PT.border}`}}>
      <span style={{display:'flex', alignItems:'center', gap:10, fontSize:13, color:PT.ink}}>
        <span style={{width:10, height:10, borderRadius:'50%', background:c, boxShadow:status==='err'?`0 0 8px ${c}`:'none'}}></span>{label}
      </span>
      <span style={{fontFamily:PT.mono, fontSize:13, color:status==='ok'?PT.ink:c, fontWeight:500}}>{value}</span>
    </div>
  );
};
const PStatus = ({alert=false}) => (
  <PFrame title="System Status" activeTab={1} state={alert?'alarm':'running'}>
    {alert && <div style={{margin:'14px 22px 0', padding:'10px 14px', background:'rgba(204,102,102,0.12)', border:`1px solid ${PT.err}`, color:PT.err, fontSize:12, fontWeight:600, letterSpacing:'0.06em', display:'flex', justifyContent:'space-between'}}><span>⚠ MQTT publish failures detected — broker unreachable for 4m 12s</span><span style={{fontFamily:PT.mono}}>12:30:44</span></div>}
    <div style={{padding:'18px 22px 50px', display:'grid', gridTemplateColumns:'1fr 1fr', columnGap:36, rowGap:0}}>
      <div>
        <div style={{fontSize:10, fontWeight:700, letterSpacing:'0.22em', color:PT.muted, textTransform:'uppercase', marginBottom:6}}>Compute</div>
        <PStatusRow label="CPU load" value="34%" status="ok"/>
        <PStatusRow label="Free heap" value="48.2 KB" status="ok"/>
        <PStatusRow label="Sensor task watermark" value="312 B" status={alert?'warn':'ok'}/>
        <PStatusRow label="MCU temperature" value="51.2 °C" status="ok"/>
        <div style={{height:14}}></div>
        <div style={{fontSize:10, fontWeight:700, letterSpacing:'0.22em', color:PT.muted, textTransform:'uppercase', marginBottom:6}}>Connectivity</div>
        <PStatusRow label="WiFi RSSI" value="−62 dBm" status="ok"/>
        <PStatusRow label="Reconnections (24h)" value={alert?'8':'0'} status={alert?'warn':'ok'}/>
        <PStatusRow label="MQTT failures" value={alert?'47':'0'} status={alert?'err':'ok'}/>
      </div>
      <div>
        <div style={{fontSize:10, fontWeight:700, letterSpacing:'0.22em', color:PT.muted, textTransform:'uppercase', marginBottom:6}}>Modbus</div>
        <PStatusRow label="Success count" value="14 829" status="ok"/>
        <PStatusRow label="CRC errors" value={alert?'12':'0'} status={alert?'warn':'ok'}/>
        <PStatusRow label="Timeouts" value={alert?'4':'0'} status={alert?'warn':'ok'}/>
        <PStatusRow label="Buffer occupancy" value="22%" status="ok"/>
        <div style={{height:14}}></div>
        <div style={{fontSize:10, fontWeight:700, letterSpacing:'0.22em', color:PT.muted, textTransform:'uppercase', marginBottom:6}}>Device</div>
        <PStatusRow label="Uptime" value="3d 14:22" status="ok"/>
        <PStatusRow label="Firmware" value="v2.4.1" status="ok"/>
        <PStatusRow label="Last config save" value="3d 14:18 ago" status="ok"/>
      </div>
    </div>
    <div style={{position:'absolute', bottom:0, left:0, right:0, padding:'10px 22px', borderTop:`1px solid ${PT.border}`, background:'#0A0E14', display:'flex', justifyContent:'space-between', fontFamily:PT.mono, fontSize:11, color:PT.muted}}>
      <span>Refreshes every 1s</span><span>{alert?'1 active alert':'All systems nominal'}</span>
    </div>
  </PFrame>
);

/* ── 9-10. ALARMS ── */
const PAlarmRow = ({sensor, dir, value, threshold, time, severity='err'}) => {
  const c = severity==='err'?PT.err:PT.warn;
  return (
    <div style={{background:PT.surf, border:`1px solid ${PT.border}`, position:'relative', padding:'14px 18px', marginBottom:10, display:'grid', gridTemplateColumns:'2fr 1fr 1fr 1fr', gap:14, alignItems:'center'}}>
      <div style={{position:'absolute', left:0, top:0, bottom:0, width:5, background:c}}></div>
      <div>
        <div style={{fontSize:14, fontWeight:600}}>{sensor} <span style={{color:c, fontWeight:700, marginLeft:6}}>{dir}</span></div>
        <div style={{fontFamily:PT.mono, fontSize:11, color:PT.muted, marginTop:3}}>Triggered {time}</div>
      </div>
      <div><div style={{fontSize:9, color:PT.muted, letterSpacing:'0.18em', fontWeight:600}}>CURRENT</div><div style={{fontFamily:PT.mono, fontSize:18, color:c, fontWeight:600}}>{value}</div></div>
      <div><div style={{fontSize:9, color:PT.muted, letterSpacing:'0.18em', fontWeight:600}}>THRESHOLD</div><div style={{fontFamily:PT.mono, fontSize:18, color:PT.ink, fontWeight:500}}>{threshold}</div></div>
      <div style={{textAlign:'right'}}><span style={{padding:'4px 10px', fontFamily:PT.mono, fontSize:10, fontWeight:700, letterSpacing:'0.16em', color:c, border:`1px solid ${c}`, background:`${c}14`}}>● ACTIVE</span></div>
    </div>
  );
};
const PAlarms = ({empty=false}) => (
  <PFrame title="Active Alarms" activeTab={2} state={empty?'running':'alarm'}>
    {empty ? (
      <div style={{height:'100%', display:'flex', flexDirection:'column', alignItems:'center', justifyContent:'center', gap:18}}>
        <div style={{width:78, height:78, borderRadius:'50%', background:'rgba(111,191,142,0.10)', display:'flex', alignItems:'center', justifyContent:'center', color:PT.ok}}>
          <PIcon name="check" size={42}/>
        </div>
        <div style={{fontSize:18, fontWeight:600, color:PT.ink}}>No active alarms</div>
        <div style={{fontFamily:PT.mono, fontSize:11, color:PT.muted, letterSpacing:'0.06em'}}>All sensors are within configured thresholds.</div>
        <div style={{fontFamily:PT.mono, fontSize:11, color:PT.muted, marginTop:14}}>Last alarm cleared 2d 4h ago</div>
      </div>
    ) : (
      <div style={{padding:18}}>
        <PAlarmRow sensor="Temperature · S1" dir="HIGH" value="42.7 °C" threshold="40.0 °C" time="12:18:07" severity="err"/>
        <PAlarmRow sensor="Pressure · S3" dir="LOW" value="982 hPa" threshold="990 hPa" time="11:54:31" severity="warn"/>
      </div>
    )}
    <div style={{position:'absolute', bottom:0, left:0, right:0, padding:'10px 22px', borderTop:`1px solid ${PT.border}`, background:'#0A0E14', display:'flex', justifyContent:'space-between', fontFamily:PT.mono, fontSize:11, color:PT.muted}}>
      <span>{empty?'0 active':'2 active'} · 0 acknowledged</span><span>Auto-refresh 1s</span>
    </div>
  </PFrame>
);

/* ── 11-13. CONFIG ── */
const PField = ({label, value, hint, error=false, focus=false, errMsg=''}) => (
  <div style={{marginBottom:14}}>
    <div style={{display:'flex', justifyContent:'space-between', marginBottom:5}}>
      <span style={{fontSize:11, color:PT.muted, fontWeight:600, letterSpacing:'0.04em'}}>{label}</span>
      {hint && <span style={{fontFamily:PT.mono, fontSize:10, color:PT.dim}}>{hint}</span>}
    </div>
    <div style={{fontFamily:PT.mono, fontSize:14, color:error?PT.err:PT.ink, padding:'9px 12px', background:PT.surf2, border:`1px solid ${error?PT.err:focus?PT.accent:PT.border}`, position:'relative'}}>{value}{focus && <span style={{position:'absolute', right:12, top:9, color:PT.accent, animation:'blink 1s steps(2) infinite'}}>|</span>}</div>
    {error && <div style={{fontSize:11, color:PT.err, marginTop:4, fontFamily:PT.mono}}>{errMsg}</div>}
  </div>
);
const PConfig = ({variant='idle'}) => {
  const isErr = variant==='invalid';
  const isModal = variant==='modal';
  return (
    <PFrame title="Configuration" activeTab={3} state="running">
      <div style={{padding:'20px 22px 60px', display:'grid', gridTemplateColumns:'1fr 1fr 1fr', gap:22}}>
        <div>
          <div style={{fontSize:10, fontWeight:700, letterSpacing:'0.22em', color:PT.accent, textTransform:'uppercase', marginBottom:14}}>Acquisition</div>
          <PField label="Poll interval" value="5000 ms" hint="100 – 60000"/>
          <PField label="Sensor bus rate" value="400 kHz" hint="100 / 400 kHz"/>
        </div>
        <div>
          <div style={{fontSize:10, fontWeight:700, letterSpacing:'0.22em', color:PT.accent, textTransform:'uppercase', marginBottom:14}}>Alarm thresholds</div>
          <PField label="Temp HIGH (°C)" value={isErr?"95.0":"40.0"} hint="−20 to 80" error={isErr} errMsg={isErr?'Out of allowed range (−20 to 80)':''}/>
          <PField label="Temp LOW (°C)" value="−5.0" hint="−20 to 80"/>
          <PField label="Hum HIGH (%RH)" value="90" hint="0 to 100"/>
        </div>
        <div>
          <div style={{fontSize:10, fontWeight:700, letterSpacing:'0.22em', color:PT.accent, textTransform:'uppercase', marginBottom:14}}>Display</div>
          <PField label="Backlight" value="78%" hint="10 – 100" focus={variant==='idle'?false:!isErr}/>
          <PField label="Theme" value="Dark · Operator"/>
          <PField label="Idle dim after" value="60 s"/>
        </div>
      </div>
      <div style={{position:'absolute', bottom:0, left:0, right:0, padding:'10px 22px', borderTop:`1px solid ${PT.border}`, background:'#0A0E14', display:'flex', justifyContent:'space-between', alignItems:'center'}}>
        <span style={{fontFamily:PT.mono, fontSize:11, color:isErr?PT.err:PT.muted}}>{isErr?'1 invalid field':'Changes pending · 4 fields modified'}</span>
        <div style={{display:'flex', gap:10}}>
          <button style={{padding:'8px 18px', background:'transparent', border:`1px solid ${PT.border}`, color:PT.muted, fontSize:11, fontWeight:700, letterSpacing:'0.16em', fontFamily:PT.sans}}>CANCEL</button>
          <button style={{padding:'8px 18px', background:isErr?PT.surf2:PT.accent, border:`1px solid ${isErr?PT.border:PT.accent}`, color:isErr?PT.dim:'#06080B', fontSize:11, fontWeight:700, letterSpacing:'0.16em', fontFamily:PT.sans}}>APPLY</button>
        </div>
      </div>
      {isModal && (
        <div style={{position:'absolute', inset:0, background:'rgba(0,0,0,0.6)', display:'flex', alignItems:'center', justifyContent:'center'}}>
          <div style={{width:380, background:PT.surf, border:`1px solid ${PT.border}`, padding:24}}>
            <div style={{display:'flex', alignItems:'center', gap:12, marginBottom:14}}>
              <span style={{color:PT.accent}}><PIcon name="shield" size={20}/></span>
              <span style={{fontSize:16, fontWeight:600}}>Save changes to flash?</span>
            </div>
            <div style={{fontSize:12, color:PT.muted, lineHeight:1.6, marginBottom:18}}>4 configuration fields will be written to persistent storage. Acquisition will pause briefly during write.</div>
            <div style={{fontFamily:PT.mono, fontSize:11, color:PT.muted, padding:'10px 12px', background:PT.bg, border:`1px solid ${PT.border}`, marginBottom:18}}>
              <div>Poll interval &nbsp; 1000 → 5000 ms</div>
              <div>Temp HIGH &nbsp;&nbsp;&nbsp; 35.0 → 40.0 °C</div>
              <div>Hum HIGH &nbsp;&nbsp;&nbsp; 85 → 90 %RH</div>
              <div>Backlight &nbsp;&nbsp;&nbsp;&nbsp; 60 → 78 %</div>
            </div>
            <div style={{display:'flex', justifyContent:'flex-end', gap:10}}>
              <button style={{padding:'8px 18px', background:'transparent', border:`1px solid ${PT.border}`, color:PT.ink, fontSize:11, fontWeight:700, letterSpacing:'0.16em', fontFamily:PT.sans}}>CANCEL</button>
              <button style={{padding:'8px 18px', background:PT.accent, border:`1px solid ${PT.accent}`, color:'#06080B', fontSize:11, fontWeight:700, letterSpacing:'0.16em', fontFamily:PT.sans}}>CONFIRM &amp; SAVE</button>
            </div>
          </div>
        </div>
      )}
    </PFrame>
  );
};

window.PScreens = {PSplash, PFirmware, PSensors, PStatus, PAlarms, PConfig};
