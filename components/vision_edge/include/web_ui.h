#ifndef WEB_UI_H
#define WEB_UI_H

const char* index_html = R"=====(
<!DOCTYPE html>
<html lang="vi">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>AIoT Receptionist</title>
    <style>
        body { font-family: 'Segoe UI', Tahoma, Geneva, Verdana, sans-serif; background-color: #121212; color: #e0e0e0; margin: 0; padding: 20px; }
        .container { max-width: 1000px; margin: auto; display: flex; flex-wrap: wrap; gap: 20px; }
        .panel { background: #1e1e1e; padding: 20px; border-radius: 12px; box-shadow: 0 4px 6px rgba(0,0,0,0.3); flex: 1; min-width: 300px; }
        h2 { margin-top: 0; color: #fff; border-bottom: 2px solid #333; padding-bottom: 10px; }
        .cam-view { text-align: center; }
        .cam-view img { max-width: 100%; border-radius: 8px; border: 2px solid #333; background: #000; min-height: 240px; }
        .status-box { background: #2a2a2a; padding: 15px; margin-top: 15px; border-radius: 8px; font-weight: bold; text-align: center; color: #4CAF50; }
        .control-group { margin-bottom: 15px; display: flex; align-items: center; justify-content: space-between; }
        input[type="text"] { width: 100%; padding: 10px; border-radius: 6px; border: 1px solid #444; background: #2a2a2a; color: #fff; margin-bottom: 10px; box-sizing: border-box; }
        button { padding: 10px 15px; border: none; border-radius: 6px; font-weight: bold; cursor: pointer; color: white; transition: 0.2s; }
        .btn-green { background-color: #4CAF50; } .btn-green:hover { background-color: #45a049; }
        .btn-red { background-color: #f44336; } .btn-red:hover { background-color: #da190b; }
        .btn-blue { background-color: #2196F3; } .btn-blue:hover { background-color: #0b7dda; width: 100%; }
        .toggle-switch { position: relative; display: inline-block; width: 50px; height: 24px; }
        .toggle-switch input { opacity: 0; width: 0; height: 0; }
        .slider { position: absolute; cursor: pointer; top: 0; left: 0; right: 0; bottom: 0; background-color: #ccc; transition: .4s; border-radius: 24px; }
        .slider:before { position: absolute; content: ""; height: 16px; width: 16px; left: 4px; bottom: 4px; background-color: white; transition: .4s; border-radius: 50%; }
        input:checked + .slider { background-color: #2196F3; }
        input:checked + .slider:before { transform: translateX(26px); }
        .person-list { max-height: 200px; overflow-y: auto; background: #2a2a2a; border-radius: 8px; padding: 10px; }
        .person-item { display: flex; justify-content: space-between; align-items: center; padding: 8px 0; border-bottom: 1px solid #444; }
        .person-item:last-child { border-bottom: none; }
        #enroll-result img { width: 100px; border-radius: 8px; margin-top: 10px; border: 2px solid #4CAF50; }
    </style>
</head>
<body>
    <div class="container">
        <!-- Khu vực Camera -->
        <div class="panel cam-view">
            <h2>Live Camera</h2>
            <img id="stream" src="" alt="Camera Stream (AI is OFF)">
            <div id="ai-status" class="status-box">Đang kết nối...</div>
        </div>

        <!-- Khu vực Điều khiển -->
        <div class="panel">
            <h2>Control Panel</h2>
            <div class="control-group">
                <span>AI Recognition</span>
                <label class="toggle-switch">
                    <input type="checkbox" id="ai-toggle" onchange="toggleAI()">
                    <span class="slider"></span>
                </label>
            </div>
            <div class="control-group">
                <span>Liveness Check</span>
                <label class="toggle-switch">
                    <input type="checkbox" id="liveness-toggle" onchange="toggleLiveness()" checked>
                    <span class="slider"></span>
                </label>
            </div>
            <button class="btn-green" style="width:100%; margin-bottom:20px; font-size:16px; padding:15px;" onclick="openDoor()">Open Door</button>

            <h2>Enroll Face</h2>
            <input type="text" id="enroll-name" placeholder="Enter name...">
            <button class="btn-blue" onclick="enrollPerson()">Enroll</button>
            <div id="enroll-result" style="text-align:center; margin-top:10px; color:#4CAF50;"></div>

            <h2 style="margin-top:20px;">Saved Faces</h2>
            <div class="person-list" id="person-list">Loading...</div>
        </div>
    </div>

    <script>
        let streamInterval = null;
        let statusInterval = null;
        let aiEnabled = false;
        let livenessEnabled = true;

        function toggleAI() {
            aiEnabled = document.getElementById('ai-toggle').checked;
            fetch(aiEnabled ? '/ai_on' : '/ai_off')
                .then(() => {
                    if (aiEnabled) {
                        document.getElementById('stream').src = '/live_frame.jpg?t=' + Date.now();
                        streamInterval = setInterval(() => {
                            document.getElementById('stream').src = '/live_frame.jpg?t=' + Date.now();
                        }, 1000);
                    } else {
                        clearInterval(streamInterval);
                        document.getElementById('stream').src = '';
                        document.getElementById('stream').alt = 'Camera Stream (AI is OFF)';
                        document.getElementById('ai-status').innerText = 'AI IS OFF';
                        document.getElementById('ai-status').style.color = '#888';
                    }
                });
        }

        function toggleLiveness() {
            livenessEnabled = document.getElementById('liveness-toggle').checked;
            fetch(livenessEnabled ? '/liveness_on' : '/liveness_off');
        }

        function fetchStatus() {
            fetch('/detect_status')
                .then(r => r.text())
                .then(txt => {
                    let st = document.getElementById('ai-status');
                    st.innerText = txt;
                    if (txt.includes('OFF')) st.style.color = '#888';
                    else if (txt.includes('ALARM') || txt.includes('FAKE')) st.style.color = '#f44336';
                    else if (txt.includes('KNOWN') || txt.includes('PASSED')) st.style.color = '#4CAF50';
                    else st.style.color = '#2196F3';
                }).catch(e => console.log(e));
        }

        function openDoor() {
            fetch('/api/door_open')
                .then(r => r.text())
                .then(t => alert('Door opened!'));
        }

        function enrollPerson() {
            let name = document.getElementById('enroll-name').value;
            if (!name) { alert('Please enter a name!'); return; }
            document.getElementById('enroll-result').innerHTML = "Enrolling face (approx 3s)...";
            fetch('/enroll?name=' + encodeURIComponent(name))
                .then(r => r.json())
                .then(data => {
                    if (data.status === 'success') {
                        document.getElementById('enroll-result').innerHTML = 
                            `✅ Enrolled: <b>${name}</b><br>` +
                            `<img src="data:image/jpeg;base64,${data.image_b64}" alt="Snapshot">`;
                        document.getElementById('enroll-name').value = '';
                        loadPersons();
                    } else {
                        document.getElementById('enroll-result').innerHTML = `❌ Error: ${data.message}`;
                    }
                }).catch(e => {
                    document.getElementById('enroll-result').innerHTML = "❌ Network Error!";
                });
        }

        function deletePerson(id) {
            if (!confirm(`Delete ID ${id}?`)) return;
            fetch('/api/persons/delete?id=' + id)
                .then(r => r.json())
                .then(data => loadPersons());
        }

        function loadPersons() {
            fetch('/api/persons')
                .then(r => r.json())
                .then(arr => {
                    let h = '';
                    arr.forEach(p => {
                        h += `<div class="person-item">
                                <span><b>${p.id}</b> - ${p.name}</span>
                                <button class="btn-red" onclick="deletePerson('${p.id}')">Delete</button>
                              </div>`;
                    });
                    if (arr.length === 0) h = 'No faces enrolled yet.';
                    document.getElementById('person-list').innerHTML = h;
                }).catch(e => document.getElementById('person-list').innerHTML = "Error loading list.");
        }

        // Start
        statusInterval = setInterval(fetchStatus, 1000);
        loadPersons();
    </script>
</body>
</html>
)=====";

#endif // WEB_UI_H
