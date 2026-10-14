# Commercial Use of Beecord

Beecord is licensed under **Apache-2.0**: commercial use is explicitly allowed. You may build
and sell products based on this repository, keep your modifications closed-source, and owe the
project nothing beyond the license conditions below.

This file lists what the license does **not** cover — the things you must handle yourself.
**Nothing here is legal advice.**

## 1. License obligations (Apache-2.0)

- Keep the LICENSE file and copyright notices in source and binary distributions.
- Mark files you modified (§4b); include the NOTICE file if one is present.
- There is no obligation to contribute back — but PRs are genuinely welcome.

## 2. Trademarks

The names and logos "Beecord" and "BuzzLink" refer to the official project only. Sell under
your own brand. Factual statements ("uses the BuzzLink protocol") are fine; implying official
endorsement or partnership is not.

## 3. Hardware compliance

- The Waveshare dev board referenced by this project is a development tool, not a
  mass-production part. For products, design your own board or use a certified ESP32-S3 module.
- Radio certification is your responsibility: SRRC (China), CE-RED (EU), FCC (US).
- Battery compliance as applicable (e.g. UN38.3 transport, GB 31241).

## 4. Patents

The project's informal search found no obviously blocking patents, but that is neither legal
advice nor a warranty. Run your own freedom-to-operate (FTO) analysis in your target markets
before commercial launch.

## 5. Privacy & recording law

Recordings are personal data in most jurisdictions (PIPL, GDPR, and various local
consent-to-record rules). You are responsible for compliance. Recommended product behavior:
local-first storage, a recording indicator (LED/tone), explicit opt-in and clear disclosure
for any cloud-AI feature, and data minimization.

## 6. Production security

- Enable ESP32-S3 **secure boot v2 + flash encryption** for mass production.
- Integrate BuzzLink's pairing token with your own account/binding system.
- Never hardcode API keys — AI endpoints must be user-configured.

## 7. Third-party protocol compatibility

This repository does **not** provide compatibility with any third-party commercial protocol or
ecosystem. If your product needs that, use the vendor's official hardware-partnership channel.
Any reverse-engineering you undertake is at your own risk.

---

# 商用须知（中文）

Beecord 采用 **Apache-2.0** 许可证：明确允许商业使用——可以基于本仓做产品并销售、修改
可以闭源、除下列许可证义务外不欠本项目任何东西。本文件列出许可证**不覆盖**、需要商用者
自行负责的事项。**本文件不构成法律意见。**

1. **许可证义务**：分发时保留 LICENSE 与版权声明；标注修改过的文件（§4b）；若有 NOTICE
   文件需一并保留。无回馈义务，但欢迎 PR。
2. **商标**：Beecord / BuzzLink 名称与标识仅指代官方项目。请用自有品牌销售；"使用
   BuzzLink 协议"这类事实性描述可以，暗示官方背书/合作不行。
3. **硬件合规**：微雪开发板是开发工具、非量产件——做产品需自研板或选用带认证的
   ESP32-S3 模组。无线电认证（国内 SRRC、欧盟 CE-RED、美国 FCC）与电池合规（UN38.3、
   GB 31241 等）由你自行负责。
4. **专利**：项目方的非正式检索未发现明显挡路专利，但不构成保证；上市前请自行做目标
   市场的 FTO 分析。
5. **隐私**：录音在多数法域属个人信息（PIPL/GDPR/各地录音同意规则）。建议：本地优先
   存储、录音指示灯/提示音、云端 AI 功能显式开启并明示、数据最小化。
6. **量产安全**：务必开启 ESP32-S3 secure boot v2 + flash encryption；BuzzLink 绑定
   token 接入自有账号体系；AI 端点 key 不得硬编码。
7. **第三方协议兼容**：本仓不提供与任何第三方商业协议/生态的兼容能力。如有需要，请走
   厂商官方硬件合作渠道；任何自行逆向的风险由你承担。
