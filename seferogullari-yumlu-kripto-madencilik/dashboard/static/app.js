let hours = 1;
let chartData = [];
let chartPoints = [];

function fmtHash(v) {
    v = Number(v || 0);

    const units = [
        ["EH/s",1e18],
        ["PH/s",1e15],
        ["TH/s",1e12],
        ["GH/s",1e9],
        ["MH/s",1e6],
        ["KH/s",1e3]
    ];

    for (const [name,n] of units) {
        if (Math.abs(v) >= n) {
            const x = v / n;
            return x.toFixed(x >= 100 ? 0 : x >= 10 ? 1 : 2) + " " + name;
        }
    }

    return v.toFixed(0) + " H/s";
}

function fmtDiff(v) {
    v = Number(v || 0);

    const units = [
        ["E",1e18],
        ["P",1e15],
        ["T",1e12],
        ["G",1e9],
        ["M",1e6],
        ["K",1e3]
    ];

    for (const [name,n] of units) {
        if (Math.abs(v) >= n) {
            const x = v / n;
            return x.toFixed(x >= 100 ? 0 : x >= 10 ? 1 : 2) + " " + name;
        }
    }

    return v.toFixed(2);
}

function fmtNumber(v) {
    return Number(v || 0).toLocaleString("tr-TR");
}

function ago(date) {
    if (!date) return "—";

    const s = Math.max(
        0,
        Math.floor(
            (Date.now() - new Date(date).getTime()) / 1000
        )
    );

    if (s < 5) return "şimdi";
    if (s < 60) return s + " sn önce";
    if (s < 3600) return Math.floor(s / 60) + " dk önce";
    if (s < 86400) return Math.floor(s / 3600) + " sa önce";

    return Math.floor(s / 86400) + " gün önce";
}

async function get(url) {
    const r = await fetch(url,{cache:"no-store"});

    if (!r.ok)
        throw new Error("HTTP " + r.status);

    return await r.json();
}

async function updateOverview() {
    try {
        const d = await get("/api/overview");

        byId("hashrate").textContent =
            fmtHash(d.hashrate_1m);

        byId("hashrate5").textContent =
            "5 dk: " + fmtHash(d.hashrate_5m);

        byId("workers").textContent =
            fmtNumber(d.workers);

        byId("best").textContent =
            fmtDiff(d.best_share);

        byId("blocks").textContent =
            fmtNumber(d.blocks);

        const dot = byId("status-dot");
        const txt = byId("status-text");

        if (d.online) {
            dot.classList.add("ok");
            txt.textContent = "Pool Aktif";
        } else {
            dot.classList.remove("ok");
            txt.textContent = "Pool Bekleniyor";
        }
    } catch(e) {
        byId("status-dot").classList.remove("ok");
        byId("status-text").textContent = "Bağlantı Yok";
    }
}

async function updateAnalytics() {
    try {
        const d = await get("/api/analytics");

        byId("accepted1h").textContent =
            fmtNumber(d.accepted_1h);

        byId("rejected1h").textContent =
            fmtNumber(d.rejected_1h);

        byId("accepted24h").textContent =
            fmtNumber(d.accepted_24h);

        byId("rejected24h").textContent =
            fmtNumber(d.rejected_24h);

        byId("avg-hashrate").textContent =
            fmtHash(d.avg_hashrate_6h);

        byId("peak-hashrate").textContent =
            fmtHash(d.peak_hashrate_6h);

        byId("last-share").textContent =
            ago(d.last_share_at);

        byId("round-diff").textContent =
            fmtDiff(d.round_diff);

        byId("network-diff").textContent =
            fmtDiff(d.network_difficulty);

        byId("network-hashrate").textContent =
            fmtHash(d.network_hashrate);

        byId("block-height").textContent =
            fmtNumber(d.block_height);

        byId("block-reward").textContent =
            Number(d.block_reward || 0).toFixed(8) + " BTC";

        const effort = Math.max(
            0,
            Number(d.round_effort_pct || 0)
        );

        byId("effort-pct").textContent =
            effort < 0.01
                ? effort.toFixed(5) + "%"
                : effort < 1
                    ? effort.toFixed(3) + "%"
                    : effort.toFixed(2) + "%";

        const ringDeg =
            Math.max(0,Math.min(effort,100)) * 3.6;

        byId("effort-ring").style.background =
            `conic-gradient(
                #f5a623 ${ringDeg}deg,
                #1a2028 ${ringDeg}deg
            )`;

    } catch(e) {}
}

async function updateMiners() {
    try {
        const rows = await get("/api/miners");
        const el = byId("miners");

        if (!rows.length) {
            el.innerHTML =
                '<tr><td colspan="5" class="empty">Madenci bekleniyor</td></tr>';
            return;
        }

        el.innerHTML = rows.map(x => `
            <tr>
                <td>
                    <span class="worker-name">
                        ${esc(x.worker_name || "worker")}
                    </span>

                    <span class="worker-address">
                        ${esc(short(x.btc_address))}
                    </span>
                </td>

                <td>${fmtHash(x.hashrate)}</td>

                <td>${fmtDiff(x.best_share_difficulty)}</td>

                <td>${ago(x.last_share_at)}</td>

                <td class="good">
                    ${esc(
                        x.status === "online"
                            ? "ONLINE"
                            : x.status || "AKTİF"
                    )}
                </td>
            </tr>
        `).join("");

    } catch(e) {}
}

async function updateBlocks() {
    try {
        const rows = await get("/api/blocks");
        const el = byId("block-list");

        if (!rows.length) {
            el.innerHTML =
                '<tr><td colspan="5" class="empty">Henüz blok bulunmadı</td></tr>';
            return;
        }

        el.innerHTML = rows.map(x => `
            <tr>
                <td>${fmtNumber(x.height)}</td>

                <td>
                    ${esc(
                        x.worker_name ||
                        short(x.btc_address)
                    )}
                </td>

                <td>
                    ${(Number(x.reward_value || 0) / 1e8)
                        .toFixed(8)} BTC
                </td>

                <td>
                    ${
                        x.round_effort == null
                            ? "—"
                            : (
                                Number(x.round_effort) /
                                Math.max(
                                    Number(x.net_difficulty || 1),
                                    1
                                ) *
                                100
                            ).toFixed(2) + "%"
                    }
                </td>

                <td>
                    ${new Date(x.found_at)
                        .toLocaleString("tr-TR")}
                </td>
            </tr>
        `).join("");

    } catch(e) {}
}

async function updateChart() {
    try {
        chartData =
            await get("/api/history?hours=" + hours);

        drawChart();
    } catch(e) {}
}

function drawChart() {
    const canvas = byId("chart");
    if (!canvas) return;

    const box = canvas.getBoundingClientRect();
    const ratio = window.devicePixelRatio || 1;

    canvas.width = Math.round(box.width * ratio);
    canvas.height = Math.round(box.height * ratio);

    const ctx = canvas.getContext("2d");
    ctx.setTransform(ratio,0,0,ratio,0,0);

    const w = box.width;
    const h = box.height;

    ctx.clearRect(0,0,w,h);

    const left = 10;
    const right = 8;
    const top = 17;
    const bottom = 24;

    const graphW = w - left - right;
    const graphH = h - top - bottom;

    ctx.lineWidth = 1;
    ctx.strokeStyle = "rgba(255,255,255,.055)";

    for (let i=0;i<5;i++) {
        const y = top + graphH * (i/4);

        ctx.beginPath();
        ctx.moveTo(left,y);
        ctx.lineTo(w-right,y);
        ctx.stroke();
    }

    if (!chartData.length) {
        ctx.fillStyle = "#596371";
        ctx.font = "12px system-ui";
        ctx.textAlign = "center";
        ctx.fillText(
            "Hashrate geçmişi oluşuyor...",
            w / 2,
            h / 2
        );

        chartPoints = [];
        return;
    }

    const values =
        chartData.map(x => Number(x.hashrate || 0));

    const maxValue =
        Math.max(...values,1) * 1.12;

    chartPoints = chartData.map((x,i) => ({
        raw: x,
        x:
            left +
            (i / Math.max(chartData.length-1,1)) *
            graphW,
        y:
            top +
            graphH -
            (Number(x.hashrate || 0) / maxValue) *
            graphH
    }));

    const grad =
        ctx.createLinearGradient(0,top,0,h-bottom);

    grad.addColorStop(0,"rgba(245,166,35,.30)");
    grad.addColorStop(.55,"rgba(245,166,35,.08)");
    grad.addColorStop(1,"rgba(245,166,35,0)");

    ctx.beginPath();
    ctx.moveTo(chartPoints[0].x,h-bottom);

    chartPoints.forEach(p =>
        ctx.lineTo(p.x,p.y)
    );

    ctx.lineTo(
        chartPoints[chartPoints.length-1].x,
        h-bottom
    );

    ctx.closePath();
    ctx.fillStyle = grad;
    ctx.fill();

    ctx.beginPath();

    chartPoints.forEach((p,i) => {
        if (i === 0)
            ctx.moveTo(p.x,p.y);
        else
            ctx.lineTo(p.x,p.y);
    });

    ctx.strokeStyle = "#f5a623";
    ctx.lineWidth = 2;
    ctx.lineJoin = "round";
    ctx.lineCap = "round";
    ctx.shadowBlur = 13;
    ctx.shadowColor = "rgba(245,166,35,.22)";
    ctx.stroke();
    ctx.shadowBlur = 0;

    const first =
        new Date(Number(chartData[0].time) * 1000);

    const last =
        new Date(
            Number(
                chartData[chartData.length-1].time
            ) * 1000
        );

    ctx.fillStyle = "#596371";
    ctx.font = "9px system-ui";

    ctx.textAlign = "left";
    ctx.fillText(
        first.toLocaleTimeString("tr-TR",{
            hour:"2-digit",
            minute:"2-digit"
        }),
        left,
        h-5
    );

    ctx.textAlign = "right";
    ctx.fillText(
        last.toLocaleTimeString("tr-TR",{
            hour:"2-digit",
            minute:"2-digit"
        }),
        w-right,
        h-5
    );
}

function chartMove(ev) {
    if (!chartPoints.length) return;

    const canvas = byId("chart");
    const rect = canvas.getBoundingClientRect();

    const x = ev.clientX - rect.left;

    let nearest = chartPoints[0];

    for (const p of chartPoints) {
        if (
            Math.abs(p.x - x) <
            Math.abs(nearest.x - x)
        ) nearest = p;
    }

    const tip = byId("chart-tooltip");

    const d =
        new Date(Number(nearest.raw.time) * 1000);

    byId("tooltip-time").textContent =
        d.toLocaleString("tr-TR",{
            hour:"2-digit",
            minute:"2-digit",
            day:"2-digit",
            month:"2-digit"
        });

    byId("tooltip-value").textContent =
        fmtHash(nearest.raw.hashrate);

    tip.style.display = "block";

    const maxLeft =
        rect.width - tip.offsetWidth - 8;

    tip.style.left =
        Math.max(
            5,
            Math.min(nearest.x + 12,maxLeft)
        ) + "px";

    tip.style.top =
        Math.max(8,nearest.y - 50) + "px";
}

function short(v) {
    if (!v) return "—";
    if (v.length < 20) return v;

    return (
        v.slice(0,9) +
        "…" +
        v.slice(-7)
    );
}

function esc(s) {
    return String(s || "")
        .replaceAll("&","&amp;")
        .replaceAll("<","&lt;")
        .replaceAll(">","&gt;")
        .replaceAll('"',"&quot;");
}

function byId(id) {
    return document.getElementById(id);
}

document
    .querySelectorAll(".ranges button")
    .forEach(btn => {
        btn.onclick = () => {
            document
                .querySelectorAll(".ranges button")
                .forEach(x =>
                    x.classList.remove("active")
                );

            btn.classList.add("active");

            hours =
                Number(btn.dataset.hours || 1);

            updateChart();
        };
    });

const chartCanvas = byId("chart");

if (chartCanvas) {
    chartCanvas.addEventListener(
        "mousemove",
        chartMove
    );

    chartCanvas.addEventListener(
        "mouseleave",
        () => {
            byId("chart-tooltip").style.display =
                "none";
        }
    );
}

async function refresh() {
    await Promise.all([
        updateOverview(),
        updateAnalytics(),
        updateMiners(),
        updateBlocks()
    ]);
}

refresh();
updateChart();

setInterval(refresh,5000);
setInterval(updateChart,30000);

window.addEventListener(
    "resize",
    drawChart
);
