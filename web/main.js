var APPKEY = '56a0a88c4407a3cd028ac2fe';
var TOPIC_REPORT = 'lock_report';
var ALIAS = 'lock_102030002';

function reset_map() {
    var map_height = $(window).height() - $('#div-map').offset().top - 48;
    // console.log('map height: ' + map_height);
    $('#div-map').height(map_height);

    map = new google.maps.Map(document.getElementById('div-map'), {
        zoom: 13,
        center: center
    });
    marker = new google.maps.Marker({
        position: center,
        map: map
    });

    info_window = new google.maps.InfoWindow();
    if (window.map_info != undefined) {
        info_window.setContent(map_info);
        info_window.open(map, marker);
    }
}

function change_map(lat, lon) {
    center = { lat: lat, lng: lon };

    var geocoder = new google.maps.Geocoder;
    geocoder.geocode({ 'location': center }, function(results, status) {
        if (status === 'OK') {
            if (results[1]) {
                map_info = results[1].formatted_address;
                reset_map();
            } else {
                console.log('no results found');
            }
        } else {
            console.log('geocoder failed due to: ' + status);
        }
    });
}

$(window).resize(function() {
    reset_map();
});

$(document).ready(function() {
    window.send_time = null;
    window.first_msg = true;

    $('#span-status').text('正在连接云巴服务器...');
    center = { lat: 22.542955, lng: 114.059688 };

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

function cell_locate(cells) {
    var cell = cells[0];
    var request = '/cellocation/cell/?mcc=' + cell.mcc + '&mnc=' + cell.mnc + '&lac=' + cell.lac + '&ci=' + cell.ci + '&output=json';
    // console.log(request);
    $.ajax(request).done(function(response) {
        console.log(response);
        if (response.errcode == 0) {
            $('#span-gps').text('位置(基站): [' + response.lat + ', ' + response.lon + ']');
            change_map(parseFloat(response.lat), parseFloat(response.lon));
        } else {
            $('#span-gps').text('位置(基站): 定位失败');
        }
    });
}

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
    } else if (msg.lock == false) {
        if (window.send_time != null) {
            var recv_time = new Date();
            var sec = (recv_time.getTime() - window.send_time.getTime()) / 1000.0;
            status = '已打开(' + sec + ' 秒)';
            window.send_time = null;
        } else {
            status = '已打开';
        }
        $('#btn-send').attr("disabled", true);
    }

    if (msg.battery != undefined) {
        battery = msg.battery;
    }
    if (msg.charge != undefined) {
        charge = msg.charge;
    }
    status += ' | 电量: ' + battery + '%';
    status += ' | 充电: ' + (charge ? '是' : '否');
    $('#span-status').text(status);

    if (msg.gps != undefined) {
        gps = msg.gps
    }
    var gps_array = gps.split(',');
    // console.log(gps_array);
    if (gps_array[6] == 0) {
        if (msg.cell != undefined) {
            $('#span-gps').text('位置(基站): 正在定位...');
            cell_locate(msg.cell);
        }
    } else {
        $('#span-gps').text('位置(GPS): [' + gps_array[2] + ', ' + gps_array[4] + ']');
        change_map(gps_array[2] / 100.0, gps_array[4] / 100.0);
    }

    if (window.first_msg == true) {
        $('#span-loading').css("display", "none");
        $('#btn-buzzer').attr("disabled", false);
        $('#btn-gps').css("display", "block");
        reset_map();
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
