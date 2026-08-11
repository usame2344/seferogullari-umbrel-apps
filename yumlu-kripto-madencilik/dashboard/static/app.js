let hours = 1;
let chartData = [];

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
        if (v >= n)
            return (v/n).toFixed(v/n >= 100 ? 0 : 2) + " " + name;
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
        if (v >= n)
            return (v/n).toFixed(v/n >= 100 ? 0 : 2) + " " + name;
    }

    return v.toFixed(2);
}

function ago(date) {
    if (!date)
        return "—";

    const s = Math.max(
        0,
        Math.floor((Date.now() - new Date(date).getTime()) / 1000)
    );

    if (s < 60) return s + " sn önce";
    if (s < 3600) return Math.floor(s/60) + " dk önce";
    if (s < 86400) return Math.floor(s/3600) + " sa önce";

    return Math.floor(s/86400) + " gün önce";
}

async function get(url) {
    const r = await fetch(url, {cache:"no-store"});
    return await r.json();
}

async function updateOverview() {
    try {
        const d = await get("/api/overview");

        document.getElementById("hashrate").textContent =
            fmtHash(d.hashrate_1m);

        document.getElementById("hashrate5").textContent =
            "5 dk: " + fmtHash(d.hashrate_5m);

        document.getElementById("workers").textContent =
            d.workers;

        document.getElementById("best").textContent =
            fmtDiff(d.best_share);

        document.getElementById("blocks").textContent =
            d.blocks;

        const dot = document.getElementById("status-dot");
        const txt = document.getElementById("status-text");

        if (d.online) {
            dot.classList.add("ok");
            txt.textContent = "Pool Aktif";
        } else {
            dot.classList.remove("ok");
            txt.textContent = "Pool Bekleniyor";
        }

    } catch(e) {}
}

async function updateMiners() {
    const rows = await get("/api/miners");
    const el = document.getElementById("miners");

    if (!rows.length) {
        el.innerHTML =
            '<tr><td colspan="5" class="empty">Henüz madenci yok</td></tr>';
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

            <td class="good">${esc(x.status || "aktif")}</td>
        </tr>
    `).join("");
}

async function updateBlocks() {
    const rows = await get("/api/blocks");
    const el = document.getElementById("block-list");

    if (!rows.length) {
        el.innerHTML =
            '<tr><td colspan="5" class="empty">Henüz blok bulunmadı</td></tr>';
        return;
    }

    el.innerHTML = rows.map(x => `
        <tr>
            <td>${x.height}</td>
            <td>${esc(x.worker_name || short(x.btc_address))}</td>
            <td>${(Number(x.reward_value || 0)/1e8).toFixed(8)} BTC</td>
            <td>${x.round_effort == null ? "—" :
                (Number(x.round_effort)*100).toFixed(2)+"%"}</td>
            <td>${new Date(x.found_at).toLocaleString("tr-TR")}</td>
        </tr>
    `).join("");
}

async function updateChart() {
    chartData = await get("/api/history?hours=" + hours);
    drawChart();
}

function drawChart() {
    const canvas = document.getElementById("chart");
    const box = canvas.getBoundingClientRect();

    const ratio = window.devicePixelRatio || 1;

    canvas.width = box.width * ratio;
    canvas.height = box.height * ratio;

    const ctx = canvas.getContext("2d");
    ctx.scale(ratio,ratio);

    const w = box.width;
    const h = box.height;

    ctx.clearRect(0,0,w,h);

    const pad = 15;

    ctx.strokeStyle = "#202630";
    ctx.lineWidth = 1;

    for(let i=1;i<5;i++) {
        const y = (h/5)*i;
        ctx.beginPath();
        ctx.moveTo(0,y);
        ctx.lineTo(w,y);
        ctx.stroke();
    }

    if (!chartData.length) {
        ctx.fillStyle = "#59616d";
        ctx.font = "13px system-ui";
        ctx.textAlign = "center";
        ctx.fillText(
            "Hashrate verisi oluştuğunda grafik burada görünecek",
            w/2,
            h/2
        );
        return;
    }

    const max = Math.max(
        ...chartData.map(x => Number(x.hashrate)),
        1
    );

    const pts = chartData.map((x,i) => ({
        x: pad + (i / Math.max(chartData.length-1,1)) * (w-pad*2),
        y: h-pad - (Number(x.hashrate)/max) * (h-pad*2)
    }));

    const grad = ctx.createLinearGradient(0,0,0,h);
    grad.addColorStop(0,"rgba(245,166,35,.26)");
    grad.addColorStop(1,"rgba(245,166,35,0)");

    ctx.beginPath();
    ctx.moveTo(pts[0].x,h-pad);

    pts.forEach(p => ctx.lineTo(p.x,p.y));

    ctx.lineTo(pts[pts.length-1].x,h-pad);
    ctx.closePath();
    ctx.fillStyle = grad;
    ctx.fill();

    ctx.beginPath();

    pts.forEach((p,i) => {
        if(i === 0) ctx.moveTo(p.x,p.y);
        else ctx.lineTo(p.x,p.y);
    });

    ctx.strokeStyle = "#f5a623";
    ctx.lineWidth = 2;
    ctx.stroke();
}

function short(v) {
    if (!v) return "—";
    if (v.length < 20) return v;
    return v.slice(0,9) + "…" + v.slice(-7);
}

function esc(s) {
    return String(s || "")
        .replaceAll("&","&amp;")
        .replaceAll("<","&lt;")
        .replaceAll(">","&gt;")
        .replaceAll('"',"&quot;");
}

document.querySelectorAll(".ranges button").forEach(btn => {
    btn.onclick = () => {
        document.querySelectorAll(".ranges button")
            .forEach(x => x.classList.remove("active"));

        btn.classList.add("active");
        hours = Number(btn.dataset.hours);
        updateChart();
    };
});

async function refresh() {
    await Promise.all([
        updateOverview(),
        updateMiners(),
        updateBlocks()
    ]);
}

refresh();
updateChart();

setInterval(refresh,5000);
setInterval(updateChart,30000);

window.addEventListener("resize",drawChart);
