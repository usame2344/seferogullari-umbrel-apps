# YUMLU KRİPTO MADENCİLİK

Umbrel için mkpool v0.3.0 tabanlı Bitcoin solo mining pool.

- Stratum V1: `3333`
- Dashboard: Umbrel **Aç** butonu / app port `8095`
- Vardiff: 1K–10M, hedef 12 share/dk
- Version rolling mask: `1fffe000`
- Coinbase signature: `/SEFEROGULLARI/`
- Donation: `%0`
- Database: TimescaleDB/PostgreSQL 17
- Dashboard: Pool hashrate, active workers, best share, block count, hashrate graph, worker details, block history

Bitcoin RPC parolası repoya yazılmaz; Umbrel Bitcoin dependency exports üzerinden runtime'da alınır.
