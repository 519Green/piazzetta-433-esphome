// piazzetta-lcd-card: the MULTICOMFORT V5 remote's screen, rebuilt from the ESPHome board's sensors.
// Layout follows the upstream button-card sample (MaximeHTL/piazzetta-433-esphome, stove-page.yaml).
// Hosted as an inline dashboard resource. Config: { type: custom:piazzetta-lcd-card, prefix?: "pellet_stove" }
class PiazzettaLcdCard extends HTMLElement {
  setConfig(config) {
    this.config = { prefix: "pellet_stove", ...config };
    if (!this.shadowRoot) this.attachShadow({ mode: "open" });
    this.shadowRoot.innerHTML = `
      <style>
        ha-card { background: #c9cfb4; border: 6px solid #1f2328; border-radius: 14px;
                  padding: 6px 12px 8px; box-shadow: inset 0 0 12px rgba(0,0,0,.25); cursor: pointer; }
        .t { display: flex; justify-content: space-between; font-size: 12px; color: #3b3fa8; letter-spacing: 1px; }
        .l { font-family: ui-monospace, Consolas, monospace; font-weight: 700; color: #3b3fa8;
             font-size: 24px; letter-spacing: 2px; min-height: 30px; white-space: pre; overflow: hidden; }
        .b { font-size: 18px; border-top: 2px solid #9aa08a; margin-top: 4px; padding-top: 2px; }
        .s { font-size: 11px; color: #5b5f6a; text-align: right; }
        .stale .l { opacity: .45; }
      </style>
      <ha-card><div class="t"><span id="p"></span><span id="r"></span><span id="f"></span></div>
        <div class="l" id="l1"></div><div class="l" id="l2"></div><div class="l b" id="bt"></div>
        <div class="s" id="age"></div></ha-card>`;
    this.shadowRoot.querySelector("ha-card").addEventListener("click", () => {
      const ev = new Event("hass-more-info", { bubbles: true, composed: true });
      ev.detail = { entityId: this.id_("sensor", "state") };
      this.dispatchEvent(ev);
    });
  }
  id_(domain, name) { return `${domain}.${this.config.prefix}_${name}`; }
  st_(domain, name) { return this._hass?.states[this.id_(domain, name)]; }
  set hass(hass) {
    this._hass = hass;
    const $ = (id) => this.shadowRoot.getElementById(id);
    const val = (d, n) => { const s = this.st_(d, n); return s && !["unknown", "unavailable"].includes(s.state) ? s.state : ""; };
    const bars = (d, n) => {
      const s = this.st_(d, n), v = parseInt(s?.state) || 0, max = parseInt(s?.attributes?.max) || 5;
      return "▮".repeat(Math.min(v, max)) + "▯".repeat(Math.max(0, max - v));
    };
    $("p").textContent = `P ${bars("number", "power")}`;
    $("f").textContent = `FAN ${bars("number", "fan")}`;
    const reach = val("binary_sensor", "stove_reachable") === "on";
    $("r").textContent = reach ? "RADIO OK" : "NO RADIO";
    $("l1").textContent = val("sensor", "screen_line_1") || "-";
    $("l2").textContent = val("sensor", "screen_line_2");
    $("bt").textContent = val("sensor", "screen_bottom");
    const age = parseFloat(val("sensor", "screen_age"));
    // Last key pressed on the real remote (sensor Remote key), with how long ago
    const rk = this.st_("sensor", "remote_key"), rkv = val("sensor", "remote_key");
    let rks = "";
    if (rkv && rk?.last_changed) {
      const m = Math.round((Date.now() - new Date(rk.last_changed).getTime()) / 60000);
      rks = `remote: ${rkv}, ${m < 60 ? m + " min" : m < 1440 ? Math.round(m / 60) + " h" : Math.round(m / 1440) + " d"} ago  ·  `;
    }
    $("age").textContent = rks + (isNaN(age) ? "" : `screen ${Math.round(age)} s old`);
    this.shadowRoot.querySelector("ha-card").classList.toggle("stale", !reach || age > 120);
  }
  getCardSize() { return 4; }
  getGridOptions() { return { columns: 12, rows: 4, min_rows: 3 }; }
}
customElements.define("piazzetta-lcd-card", PiazzettaLcdCard);
window.customCards = window.customCards || [];
window.customCards.push({ type: "piazzetta-lcd-card", name: "Piazzetta LCD", description: "Pellet stove remote screen" });
