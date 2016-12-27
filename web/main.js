var APPKEY = '56a0a88c4407a3cd028ac2fe';
var TOPIC_REPORT = 'lock_report';
var ALIAS = 'lock_102030002';


$(document).ready(function() {
    window.send_time = null;
    window.first_msg = true;
    $('#span-status').text('正在连接云巴服务器...');

    window.yunba = new Yunba({
        server: 'sock.yunba.io',
        port: 3000,
        appkey: APPKEY
    });

    // 初始化云巴 SDK
    yunba.init(function(success) {
        if (success) {
            var cid = Math.random().toString().substr(2);
            console.log('cid: ' + cid);
            window.alias = cid;

            // 连接云巴服务器
            yunba.connect_by_customid(cid,
                function(success, msg, sessionid) {
                    if (success) {
                        console.log('sessionid：' + sessionid);

                        // 设置收到信息回调函数
                        yunba.set_message_cb(yunba_msg_cb);
                        // TOPIC
                        yunba.subscribe({
                                'topic': TOPIC_REPORT
                            },
                            function(success, msg) {
                                if (success) {
                                    console.log('subscribed');
                                    yunba_sub_ok();
                                } else {
                                    console.log(msg);
                                }
                            }
                        );
                    } else {
                        console.log(msg);
                    }
                });
        } else {
            console.log('yunba init failed');
        }
    });
});

$('#btn-send').click(function() {
    var msg = JSON.stringify({ cmd: "unlock" });
    yunba.publish_to_alias({
        'alias': ALIAS,
        'msg': msg,
    }, function(success, msg) {
        if (!success) {
            console.log(msg);
        }
    });
    window.send_time = new Date();
});

$('#btn-buzzer').click(function() {
    var msg = JSON.stringify({ cmd: "buzzer" });
    yunba.publish_to_alias({
        'alias': ALIAS,
        'msg': msg,
    }, function(success, msg) {
        if (!success) {
            console.log(msg);
        }
    });
});


function yunba_msg_cb(data) {
    console.log(data);
    if (data.topic != TOPIC_REPORT) {
        return;
    }

    var msg = JSON.parse(data.msg);
    var status = ""
    if (msg.lock == true) {
        status = '已锁上';
        $('#btn-send').attr("disabled", false);
    } else {
        if (window.send_time != null) {
            var recv_time = new Date();
            var sec = (recv_time.getTime() - window.send_time.getTime()) / 1000.0;
            status = '已打开(用时 ' + sec + ' 秒)';
            window.send_time = null;
        } else {
            status = '已打开';
        }
        $('#btn-send').attr("disabled", true);
    }

    status += ' | 电量: ' + msg.battery + '%';
    status += ' | 充电: ' + (msg.charge ? '是' : '否');
    $('#span-status').text(status);

    var gps = msg.gps.split(',');
    console.log(gps);
    if (gps[6] == 0) {
        $('#span-gps').text('位置: 不可定位 | 可见卫星数: ' + gps[7]);
    } else {
        $('#span-gps').text('位置: [' + gps[2] + 'N, ' + gps[4] + 'E]');
    }

    if (window.first_msg == true) {
        $('#span-loading').css("display", "none");
        $('#btn-buzzer').attr("disabled", false);
        $('#btn-gps').css("display", "block");
        window.first_msg = false;
    }
}

function yunba_sub_ok() {
    $('#span-status').text('正在获取锁的状态...');
    var msg = JSON.stringify({ cmd: "report" });
    yunba.publish_to_alias({
        'alias': ALIAS,
        'msg': msg,
    }, function(success, msg) {
        if (!success) {
            console.log(msg);
        }
    });
}
