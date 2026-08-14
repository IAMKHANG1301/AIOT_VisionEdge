#ifndef WEB_UI_H
#define WEB_UI_H

const char* index_html = R"=====(
<!DOCTYPE html>
<html lang="vi">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>AIoT Receptionist</title>
    <link href="https://fonts.googleapis.com/css2?family=Inter:wght@300;400;600;800&display=swap" rel="stylesheet">
    <style>
        :root {
            --bg-color: #0f172a;
            --panel-bg: rgba(30, 41, 59, 0.7);
            --panel-border: rgba(255, 255, 255, 0.1);
            --text-main: #f8fafc;
            --text-muted: #94a3b8;
            --accent-primary: #6366f1;
            --accent-hover: #4f46e5;
            --accent-green: #10b981;
            --accent-red: #ef4444;
            --accent-purple: #8b5cf6;
        }
        body { 
            font-family: 'Inter', sans-serif; 
            background: radial-gradient(circle at top right, #1e1b4b, #0f172a);
            color: var(--text-main); 
            margin: 0; 
            padding: 20px; 
            min-height: 100vh;
            box-sizing: border-box;
        }
        h2 { 
            margin-top: 0; 
            font-weight: 600;
            font-size: 1.2rem;
            color: #e2e8f0;
            border-bottom: 1px solid var(--panel-border);
            padding-bottom: 12px;
            margin-bottom: 15px;
        }
        .main-layout {
            display: flex;
            gap: 25px;
            max-width: 1200px;
            margin: 0 auto;
            flex-wrap: wrap;
        }
        .col-left {
            flex: 1.2;
            display: flex;
            flex-direction: column;
            gap: 20px;
            min-width: 320px;
        }
        .col-right {
            flex: 1;
            min-width: 320px;
            display: flex;
            flex-direction: column;
        }
        .card {
            background: var(--panel-bg);
            backdrop-filter: blur(12px);
            -webkit-backdrop-filter: blur(12px);
            border: 1px solid var(--panel-border);
            border-radius: 16px;
            padding: 20px;
            box-shadow: 0 10px 25px -5px rgba(0, 0, 0, 0.5);
        }
        .h-full { flex: 1; display: flex; flex-direction: column; }
        
        /* Camera */
        .cam-container {
            width: 100%;
            border-radius: 12px;
            overflow: hidden;
            background: #000;
            position: relative;
            min-height: 240px;
            border: 1px solid var(--panel-border);
            box-shadow: 0 4px 6px rgba(0,0,0,0.3);
        }
        .cam-container img {
            width: 100%;
            display: block;
            object-fit: cover;
        }
        .status-badge {
            background: rgba(16, 185, 129, 0.2);
            color: var(--accent-green);
            padding: 12px;
            border-radius: 8px;
            text-align: center;
            font-weight: 600;
            margin-top: 15px;
            border: 1px solid rgba(16, 185, 129, 0.3);
            transition: all 0.3s ease;
        }
        
        /* Controls */
        .control-row {
            display: flex;
            justify-content: space-between;
            align-items: center;
            margin-bottom: 15px;
            padding: 10px;
            background: rgba(0,0,0,0.2);
            border-radius: 10px;
        }
        .toggle-switch { position: relative; display: inline-block; width: 54px; height: 28px; }
        .toggle-switch input { opacity: 0; width: 0; height: 0; }
        .slider { position: absolute; cursor: pointer; top: 0; left: 0; right: 0; bottom: 0; background-color: #475569; transition: .4s; border-radius: 34px; }
        .slider:before { position: absolute; content: ""; height: 20px; width: 20px; left: 4px; bottom: 4px; background-color: white; transition: .4s; border-radius: 50%; box-shadow: 0 2px 4px rgba(0,0,0,0.2); }
        input:checked + .slider { background-color: var(--accent-primary); }
        input:checked + .slider:before { transform: translateX(26px); }
        
        .btn-group { display: flex; gap: 10px; }
        button {
            flex: 1;
            padding: 12px 15px;
            border: none;
            border-radius: 8px;
            font-weight: 600;
            font-family: inherit;
            cursor: pointer;
            color: white;
            transition: all 0.2s ease;
            box-shadow: 0 4px 6px rgba(0,0,0,0.1);
        }
        button:active { transform: scale(0.98); }
        .btn-green { background: linear-gradient(135deg, #10b981, #059669); }
        .btn-green:hover { background: linear-gradient(135deg, #34d399, #10b981); }
        .btn-purple { background: linear-gradient(135deg, #8b5cf6, #6d28d9); }
        .btn-purple:hover { background: linear-gradient(135deg, #a78bfa, #8b5cf6); }
        .btn-primary { background: linear-gradient(135deg, #6366f1, #4f46e5); }
        .btn-primary:hover { background: linear-gradient(135deg, #818cf8, #6366f1); }
        .btn-danger { background: rgba(239, 68, 68, 0.2); color: #ef4444; box-shadow: none; padding: 6px 12px; font-size: 0.9rem; border: 1px solid rgba(239, 68, 68, 0.3); flex: none; }
        .btn-danger:hover { background: rgba(239, 68, 68, 0.3); }

        /* Database */
        .enroll-box {
            display: flex;
            gap: 10px;
            margin-bottom: 15px;
        }
        input[type="text"] {
            flex: 2;
            padding: 12px;
            border-radius: 8px;
            border: 1px solid var(--panel-border);
            background: rgba(0,0,0,0.2);
            color: white;
            font-family: inherit;
            outline: none;
            transition: border 0.3s;
        }
        input[type="text"]:focus { border-color: var(--accent-primary); }
        
        .person-list {
            max-height: 220px;
            overflow-y: auto;
            display: flex;
            flex-direction: column;
            gap: 8px;
        }
        .person-list::-webkit-scrollbar { width: 6px; }
        .person-list::-webkit-scrollbar-thumb { background: #475569; border-radius: 4px; }
        .person-item {
            display: flex;
            justify-content: space-between;
            align-items: center;
            padding: 12px;
            background: rgba(255,255,255,0.03);
            border: 1px solid var(--panel-border);
            border-radius: 8px;
        }
        #enroll-result img { width: 80px; border-radius: 8px; margin-top: 10px; border: 2px solid var(--accent-green); }
        
        /* Logs */
        .log-container {
            flex: 1;
            overflow-y: auto;
            display: flex;
            flex-direction: column;
            gap: 12px;
            padding-right: 5px;
            max-height: 600px;
        }
        .log-container::-webkit-scrollbar { width: 6px; }
        .log-container::-webkit-scrollbar-thumb { background: #475569; border-radius: 4px; }
        
        .log-item {
            display: flex;
            flex-direction: column;
            padding: 12px 16px;
            border-radius: 10px;
            background: rgba(0,0,0,0.2);
            border-left: 4px solid #475569;
            animation: slideIn 0.3s ease-out;
        }
        @keyframes slideIn {
            from { opacity: 0; transform: translateX(20px); }
            to { opacity: 1; transform: translateX(0); }
        }
        .log-time {
            font-size: 0.8rem;
            color: var(--text-muted);
            margin-bottom: 4px;
        }
        .log-info {
            display: flex;
            align-items: center;
            gap: 10px;
        }
        .log-type {
            font-size: 0.75rem;
            font-weight: 800;
            text-transform: uppercase;
            padding: 3px 8px;
            border-radius: 4px;
            letter-spacing: 0.5px;
        }
        .log-name {
            font-weight: 600;
            font-size: 1.05rem;
        }
        
        /* Modifiers */
        .log-known { border-left-color: var(--accent-green); background: linear-gradient(90deg, rgba(16, 185, 129, 0.1) 0%, rgba(0,0,0,0.2) 100%); }
        .log-known .log-type { background: rgba(16, 185, 129, 0.2); color: var(--accent-green); }
        
        .log-unknown { border-left-color: var(--accent-red); background: linear-gradient(90deg, rgba(239, 68, 68, 0.1) 0%, rgba(0,0,0,0.2) 100%); }
        .log-unknown .log-type { background: rgba(239, 68, 68, 0.2); color: var(--accent-red); }
        
    </style>
</head>
<body>
    <div class="main-layout">
        <!-- COL LEFT: CAM, CONTROLS, DB -->
        <div class="col-left">
            <div class="card">
                <div style="display: flex; justify-content: space-between; align-items: center; border-bottom: 1px solid var(--panel-border); margin-bottom: 15px; padding-bottom: 12px;">
                    <h2 style="border: none; margin: 0; padding: 0;">LIVESTREAM CAMERA</h2>
                    <div style="display: flex; align-items: center; gap: 10px;">
                        <span style="font-weight:600; font-size:0.9rem;">Bật AI</span>
                        <label class="toggle-switch">
                            <input type="checkbox" id="ai-toggle" onchange="toggleAI()">
                            <span class="slider"></span>
                        </label>
                    </div>
                </div>
                <div class="cam-container">
                    <img id="stream" src="" alt="Camera Stream (AI is OFF)">
                </div>
                <div id="ai-status" class="status-badge">AI IS OFF</div>
                <div id="sonar-status" class="status-badge" style="margin-top: 10px; background-color: rgba(239, 68, 68, 0.2); color: #ef4444; border: 1px solid rgba(239, 68, 68, 0.3);">Khoảng cách: -- cm</div>
            </div>
            
            <div class="card" style="display: none;">
                <h2>BẢNG ĐIỀU KHIỂN</h2>
                <div class="control-row">
                    <span style="font-weight:600;">Hệ thống Nhận diện (AI)</span>
                    <label class="toggle-switch">
                        <input type="checkbox" onchange="toggleAI()">
                        <span class="slider"></span>
                    </label>
                </div>
                <div class="btn-group">
                    <button class="btn-green" onclick="openDoor()">🔓 MỞ CỬA</button>
                    <button class="btn-purple" onclick="testAudio()" id="btn-audio-test">🎙️ THỬ VOICE</button>
                </div>
            </div>
            
            <div class="card">
                <h2>QUẢN LÝ NGƯỜI QUEN</h2>
                <div class="enroll-box">
                    <input type="text" id="enroll-name" placeholder="Nhập tên người quen...">
                    <button class="btn-primary" onclick="enrollPerson()" style="flex:1;">Thêm Khuôn Mặt</button>
                </div>
                <div id="enroll-result" style="text-align:center; font-size:0.9rem; margin-bottom:10px; min-height:20px;"></div>
                <div class="person-list" id="person-list">
                    <div style="text-align:center; color:#94a3b8; padding:20px;">Đang tải dữ liệu...</div>
                </div>
            </div>

            <!-- 
            <div class="card">
                <h2>THU THẬP DATASET TẠM THỜI</h2>
                <div style="font-size:0.85rem; color:#94a3b8; margin-bottom:10px;">Chụp 100 tấm ảnh để huấn luyện thuật toán chống giả mạo.</div>
                <div class="btn-group" style="margin-bottom: 10px;">
                    <button class="btn-green" onclick="startCapture('real')" id="btn-cap-real">Chụp 100 Ảnh Real</button>
                    <button class="btn-danger" style="flex:1; border-radius:8px;" onclick="startCapture('fake')" id="btn-cap-fake">Chụp 100 Ảnh Fake</button>
                </div>
                <button class="btn-purple" style="width: 100%;" onclick="downloadDataset()">⬇️ Tải về Dataset (.tar) & Xóa</button>
                <div id="dataset-status" style="text-align:center; font-size:0.9rem; margin-top:10px; color:var(--accent-primary);"></div>
            </div>
            -->
        </div>

        <!-- COL RIGHT: LOGS -->
        <div class="col-right">
            <div class="card h-full">
                <div style="display: flex; justify-content: space-between; align-items: center; border-bottom: 1px solid var(--panel-border); margin-bottom: 15px; padding-bottom: 12px;">
                    <h2 style="border: none; margin: 0; padding: 0;">NHẬT KÝ NHẬN DIỆN & MỞ CỬA</h2>
                    <button class="btn-danger" style="padding: 4px 10px; font-size: 0.8rem; border-radius: 6px;" onclick="clearLogs()">Xóa Nhật Ký</button>
                </div>
                <div class="log-container" id="log-list">
                    <div style="text-align:center; color:#94a3b8; padding:20px;">Đang tải nhật ký...</div>
                </div>
            </div>
        </div>
    </div>

    <script>
        let aiEnabled = false;
        let personsMap = {}; // ID -> Name map

        function toggleAI() {
            aiEnabled = document.getElementById('ai-toggle').checked;
            fetch(aiEnabled ? '/ai_on' : '/ai_off')
                .then(() => {
                    if (aiEnabled) {
                        document.getElementById('stream').src = 'http://' + window.location.hostname + ':81/stream';
                    } else {
                        document.getElementById('stream').src = '';
                        document.getElementById('stream').alt = 'Camera Stream (AI is OFF)';
                        updateStatusBadge('AI IS OFF', '#94a3b8', 'rgba(148, 163, 184, 0.2)');
                    }
                });
        }

        function updateStatusBadge(text, color, bgColor) {
            let st = document.getElementById('ai-status');
            st.innerText = text;
            st.style.color = color;
            st.style.backgroundColor = bgColor;
            st.style.borderColor = color.replace(')', ', 0.3)').replace('rgb', 'rgba');
        }

        function fetchStatus() {
            if (!aiEnabled) return;
            fetch('/detect_status')
                .then(r => r.text())
                .then(txt => {
                    if (txt.includes('OFF')) updateStatusBadge(txt, '#94a3b8', 'rgba(148, 163, 184, 0.2)');
                    else if (txt.includes('SPOOFING')) updateStatusBadge(txt, '#ef4444', 'rgba(239, 68, 68, 0.2)'); // Red
                    else if (txt.includes('STRANGER')) updateStatusBadge(txt, '#f97316', 'rgba(249, 115, 22, 0.2)'); // Dark Orange
                    else if (txt.includes('KNOWN') || txt.includes('PASSED')) updateStatusBadge(txt, '#10b981', 'rgba(16, 185, 129, 0.2)');
                    else updateStatusBadge(txt, '#6366f1', 'rgba(99, 102, 241, 0.2)');
                }).catch(e => console.log(e));
        }

        function fetchSonar() {
            fetch('/api/sonar')
                .then(r => r.json())
                .then(data => {
                    let st = document.getElementById('sonar-status');
                    st.innerText = 'Khoảng cách: ' + data.distance + ' cm';
                    if (data.distance < 100) {
                        st.style.color = '#10b981';
                        st.style.backgroundColor = 'rgba(16, 185, 129, 0.2)';
                        st.style.borderColor = 'rgba(16, 185, 129, 0.3)';
                    } else {
                        st.style.color = '#ef4444';
                        st.style.backgroundColor = 'rgba(239, 68, 68, 0.2)';
                        st.style.borderColor = 'rgba(239, 68, 68, 0.3)';
                    }
                }).catch(e => console.log(e));
        }

        function openDoor() {
            fetch('/api/door_open')
                .then(r => r.text())
                .then(t => alert('Đã gửi lệnh mở cửa!'));
        }

        function testAudio() {
            let btn = document.getElementById('btn-audio-test');
            fetch('/api/test_audio')
                .then(r => r.json())
                .then(data => {
                    if (data.is_recording) {
                        btn.innerText = 'Đang ghi âm (Bấm lại để Ngắt)';
                        btn.style.background = 'linear-gradient(135deg, #ef4444, #dc2626)';
                    } else {
                        btn.innerText = 'Đang xử lý...';
                        btn.style.background = 'linear-gradient(135deg, #f59e0b, #d97706)';
                        setTimeout(() => {
                            btn.innerText = '🎙️ THỬ VOICE';
                            btn.style.background = '';
                        }, 8000);
                    }
                }).catch(e => {
                    alert('Lỗi: ' + e);
                    btn.innerText = '🎙️ THỬ VOICE';
                    btn.style.background = '';
                });
        }

        function enrollPerson() {
            let name = document.getElementById('enroll-name').value;
            if (!name) { alert('Vui lòng nhập tên!'); return; }
            let res = document.getElementById('enroll-result');
            res.innerHTML = "<span style='color:var(--accent-primary)'>Đang lấy khuôn mặt (chờ 3s)...</span>";
            fetch('/enroll?name=' + encodeURIComponent(name))
                .then(r => r.json())
                .then(data => {
                    if (data.status === 'success') {
                        res.innerHTML = `<span style='color:var(--accent-green)'>✅ Đã thêm: <b>${name}</b></span>`;
                        document.getElementById('enroll-name').value = '';
                        loadPersons();
                    } else {
                        res.innerHTML = `<span style='color:var(--accent-red)'>❌ Lỗi: ${data.message}</span>`;
                    }
                }).catch(e => {
                    res.innerHTML = "<span style='color:var(--accent-red)'>❌ Lỗi mạng!</span>";
                });
        }

        function deletePerson(id) {
            if (!confirm(`Xóa khuôn mặt ID ${id}?`)) return;
            fetch('/api/persons/delete?id=' + id)
                .then(r => r.json())
                .then(data => loadPersons());
        }

        function clearLogs() {
            if (!confirm("Bạn có chắc chắn muốn xóa toàn bộ nhật ký (kể cả trong thẻ nhớ) không?")) return;
            fetch('/api/logs/clear')
                .then(r => r.json())
                .then(data => {
                    lastLogsJSON = ""; // force refresh
                    loadLogs();
                });
        }

        // --- DATASET COLLECTION ---
        function startCapture(type) {
            let label = type === 'real' ? "Real" : "Fake";
            if (!confirm(`Hệ thống sẽ bắt đầu chụp 100 ảnh ${label}. Bạn hãy liên tục chuyển động đầu trong khoảng 30 giây để lấy góc mặt nhé. Đồng ý?`)) return;
            
            fetch('/api/dataset/capture?type=' + type)
                .then(r => r.json())
                .then(data => {
                    document.getElementById('dataset-status').innerText = `Đang bắt đầu chụp 100 ảnh ${label}...`;
                }).catch(e => alert("Lỗi kết nối"));
        }

        function checkDatasetStatus() {
            fetch('/api/dataset/status')
                .then(r => r.json())
                .then(data => {
                    isCapturing = data.capturing;
                    if (data.capturing) {
                        let label = data.type === 'real' ? "Real" : "Fake";
                        document.getElementById('dataset-status').innerText = `📸 Đang chụp ảnh ${label}: ${data.count} / 100`;
                    } else if (data.count === 100) {
                        document.getElementById('dataset-status').innerText = `✅ Đã chụp xong 100 ảnh ${data.type === 'real' ? "Real" : "Fake"}! Bạn có thể tải về.`;
                    }
                }).catch(e => {});
        }
        setInterval(checkDatasetStatus, 1000);

        function downloadDataset() {
            window.location.href = '/api/dataset/download';
            setTimeout(() => {
                document.getElementById('dataset-status').innerText = "Đã tải về và xóa dữ liệu trên thẻ nhớ.";
            }, 3000);
        }

        function loadPersons() {
            fetch('/api/persons')
                .then(r => r.json())
                .then(arr => {
                    personsMap = {};
                    let h = '';
                    arr.forEach(p => {
                        personsMap[p.id] = p.name;
                        h += `<div class="person-item">
                                <div><b style="color:var(--accent-primary)">${p.id}</b> - ${p.name}</div>
                                <button class="btn-danger" onclick="deletePerson('${p.id}')">Xóa</button>
                              </div>`;
                    });
                    if (arr.length === 0) h = '<div style="text-align:center; color:#94a3b8; padding:20px;">Chưa có dữ liệu</div>';
                    document.getElementById('person-list').innerHTML = h;
                    
                    // After updating persons, update logs to reflect names
                    loadLogs();
                }).catch(e => document.getElementById('person-list').innerHTML = "Lỗi tải dữ liệu.");
        }

        let lastLogsJSON = "";
        let isCapturing = false;
        
        function loadLogs() {
            if (isCapturing) return; // DON'T ACCESS SD CARD WHILE CAPTURING
            
            fetch('/api/logs')
                .then(r => r.json())
                .then(arr => {
                    let currentJSON = JSON.stringify(arr);
                    if (currentJSON === lastLogsJSON && Object.keys(personsMap).length > 0) return; // No change
                    lastLogsJSON = currentJSON;
                    
                    let h = '';
                    // Reverse to show newest first
                    arr.reverse().forEach(log => {
                        let date = new Date(log.timestamp * 1000).toLocaleString('vi-VN');
                        let eventStr = log.event;
                        let name = "Người lạ";
                        let colorClass = "log-unknown";
                        
                        if (eventStr === 'known') {
                            let personName = personsMap[log.person_id] || "Người quen";
                            name = `[${log.person_id}] ${personName}`;
                            colorClass = "log-known";
                            eventStr = "Quen";
                        } else if (eventStr === 'spoof') {
                            let personName = personsMap[log.person_id] || "Người quen";
                            name = `[${log.person_id}] ${personName}`;
                            colorClass = "log-unknown"; // Màu đỏ
                            eventStr = "Giả mạo";
                        } else if (eventStr === 'unknown') {
                            eventStr = "Lạ";
                        } else {
                            eventStr = "Lạ"; // fallback
                        }
                        
                        h += `<div class="log-item ${colorClass}">
                                <div class="log-time">${date}</div>
                                <div class="log-info">
                                    <span class="log-type">${eventStr}</span>
                                    <span class="log-name">${name}</span>
                                </div>
                              </div>`;
                    });
                    if (arr.length === 0) h = '<div style="text-align:center; color:#94a3b8; padding:20px;">Nhật ký trống</div>';
                    document.getElementById('log-list').innerHTML = h;
                });
        }

        // Start loops
        setInterval(fetchStatus, 1000);
        setInterval(fetchSonar, 1000);
        setInterval(loadLogs, 2000);
        loadPersons();
    </script>
</body>
</html>
)=====";

#endif // WEB_UI_H
