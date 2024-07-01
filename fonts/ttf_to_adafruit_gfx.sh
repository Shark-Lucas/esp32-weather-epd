#!/bin/bash
# Font file to Adafruit GFX format script for esp32-weather-epd.


TTF_FILES=ttf/*
OUTPUT_PATH="./fonts"
SIZES=(4 5 6 7 8 9 10 11 12 14 16 18 20 22 24 26)
TEMPERATURE_SIZES=(48)

# clean fonts output
echo "Cleaning $OUTPUT_PATH"
rm -r $OUTPUT_PATH
mkdir $OUTPUT_PATH

CHINESE=一七三不严中九二五优低体信八六内出北区十千压可号同周四夏外失好室市度弱强很感接无日时星月期极染步武气汉江池污温湖湿用电省秀空等米紫线络网能良落见象败质轻连重量间风°
CHINESE_ALL=汤骅琅牌晴塞佛怒深观台蔚漠冶则疏镶亨相故休渭盛义荔葛油巴斡姑为隅瓯迭省堡海场畲鸠称邑芷成阳达松圳乃伦应宏遂陀邡舟禹绍百从部彬丰浮邗掇容各滋繁拐穴平莒漯德霄紫碱陈麻坊蓬电分极桥含确蔡千辖吾寨开班带乐涂郯乔鲜鄂碚香远口布岫防州科伊征寻阡措钟磨花迎闵章端家烽合令麓滁巩峦孜舆盂石郸奎依乌级西地吕棱闽个黑崇勉拉岱定良郭岑望尼岷心汨长晋师普比犍蚌葫兴邛湛蕲务改位枞荣汀解萝歙益陶羌错资林洱镇舒垫邕卡塔昭遥浪浔卫新炎淅黟巢烈蒲锋聊晃斯孝稻指延陵结猗陇华六桦嫩象陟傈易卓魏天榆涧里树汝施郏风恰凰藏业苑娄运团敏波如谟功原穆交铜庐全三北田乾敖梅固皮阊度集桂盟碾振略浏荷龙潘管汕梧芳汾区涟胡召那郎浠夹旺习左佳高丽主国扶岔豫外监群类潞霍菏忠晏牙手醴蔺劝阿孙仑英攸棉处凌末盈润谊查涞办冈梓麟僳行青洮汉社郾伽厦彝塘泰察秭傣后芒烟拜次礼边潢诏稷船宜钢濠揭耿居讷汇玄济颇充巍浙九卢静万岐治登元格淮涉坪戈沙涿张荥番衢磁八植克邻穗祜隆独归息瑶柏沂壁寒索春鄯杂枝叶内芮萨沧鲅肃池康沈淞经肇右仪璧峄佤芙禅煌鲁汪焦野哈婺范荆浚鞍等坛罘寿撒邢理阎砚太翼随锡驿栖贵上滕仫节户宕彩霞志庆重园驻牡禺简游堆锦宿莫宛君雅绵循恭邺柘崃眉玛麒脱耀澳灯沁吉至招赫始芜提岛尾力洛旧澧李米铅祁薛淀鸡阜霸沭栗靖孚嘎朗湟潜埠恒吐瑞堰勃濉贤岢芗杨咸丹谋盘昆单隰阴道樟毕连余峰载泌杏洼乡琼印鹰和碑屿封邯熟禾轮小启横汶得琊都屯皋未广喀博神秀梦土郊融夷韶箭屏店黔仁中尖下圣凭留氏彦桐郴共宣环尚眙清玉鄞朝岩楼孟进越拱夏巧福垒周昌疆会巫京河宇银鸭沽坡椒茅贺大盖滩楞兖赞金衡壶闻包绿怀们项增且日桓茶畴邵放圈房朐好觉藤贾底襄棣同公冀冠特沐许利邙湄来彰谷修红叠坻莞凉珙碌谦果弋年潍勤军榕邳朔曙源负勐灌仲庄景曼昂珲冷回慈迦加头名澄研基仓诸考照渑闸脂蕴奉临侯感宫流市辛鄢炉谢酒蒙王直托赉莘恩子突步助武秦蓉谯潭佬馆梁宾鹤句槐甸颍柱仡族枣湾沃铁芦呼硕尔鼓阆什精昔奈浈刚淳音虞扬偃黄芬门离绥任莆覃白图郁曹呈联萧崂旬额要伟维潮丘勒当儋首版喇奇匀顶车曲沾积让牟郫壤顺五美亚莱蓟强岸坤莎灰府磴苗株嵩尧峙南向十雁巨草宝淇滴莹禄藁岭厢岗腾噶廊澜遵厂喜茌光事满曾涵狮沿叙邮睢瓮徐前常盱足攀辉町杭翠老献嵊岳册罗冲濮立革桃明川陉苍信侗贞港溪镜郧古饶茂泗溆冕宽织辰鱼渝竹作库莲桑焉纳阁辽代岚策淄营宗雨二杜正姜耒烦审酉腊迪砀卧舞泾洋村邱界灞七牛珠农政密江乳雍浩胜谱旌囊泊复逊斗井费钦丈蝶间射虎漳盐溧赣缙鲤犁梨杞化富弓犹忻峡建窑渡浦戴马雷祥准权善渠唐多聂滑柞丁站濞互寺仙墉街山埔官泉承即点虹温鹿永月峻郓赤津本鸣吴惠刀凯栾翔肥耆两儿韩双思漾忧真鄄麦木瓦羊陂扎沛陆悟友干工将廛根无暨仆垦徽若尉荫色于迈筠柳拖楚弥水湖关东四云峨矿爱淖久洞民蓝畹札商迁嘴柔城路贝细綦沟票彭蛟星皇旅甘附绛樊廉方邓剑贡亭雄潼赵蒗凤旗默兰微响泸围陕央垣坎伍苏齐介蕉列获滦茄尤柯墨壮露墅洪徒申绩祝抚架姚威文裕灵安黎戚湘沅坝宁秉通郑历嘉浑罕偏保伯县掖芝毛自磐滨邹洲堂敦胶赛丛翁蠡鼎庵萍涪峪泽法一不严优低体出压可号失室弱很接时期染气污湿用空线络网能落见败质轻量°

for fontfile in $TTF_FILES
  do
  # convert .otf/.ttf files to c-style arrays
  FONT=`basename ${fontfile%%.*} | tr '-' '_'`
  echo $FONT
  mkdir $OUTPUT_PATH/$FONT
  for SI in ${SIZES[*]}
    do
    OUTFILE=$OUTPUT_PATH/$FONT/$FONT"_"$SI"pt8b.h"
	if [ $SI -eq 16 ]; then
		echo "fontconvert ${fontfile} $SI $CHINESE_ALL > $OUTFILE"
		./fontconvert ${fontfile} $SI $CHINESE_ALL > $OUTFILE
	else
		echo "fontconvert ${fontfile} $SI $CHINESE > $OUTFILE"
		./fontconvert ${fontfile} $SI $CHINESE > $OUTFILE
	fi
    sed -i "s/${SI}pt8b/_${SI}pt8b/g" $OUTFILE
    # sed -i "s/_remap${SI}pt8b/${SI}pt8b/g" $OUTFILE
  done

  for SI in ${TEMPERATURE_SIZES[*]}
    do
    OUTFILE=$OUTPUT_PATH/$FONT/$FONT"_"$SI"pt8b_temperature.h"
    echo "fontconvert ${fontfile} $SI $CHINESE > $OUTFILE"
    ./fontconvert ${fontfile} $SI $CHINESE > $OUTFILE
    sed -i "s/_temperature_set${SI}pt8b/_${SI}pt8b_temperature/g" $OUTFILE
  done

  # create header file (this will make fonts way easier to include)
  HEADER_FILE=$OUTPUT_PATH/$FONT".h"
  echo "#ifndef __FONTS_"${FONT^^}"_H__" >> $HEADER_FILE
  echo "#define __FONTS_"${FONT^^}"_H__" >> $HEADER_FILE
  for FILE in $OUTPUT_PATH/$FONT/*
    do
    echo "#include \"$FONT/`basename $FILE`\"" >> $HEADER_FILE
  done
  echo "" >> $HEADER_FILE
  for FILE in $OUTPUT_PATH/$FONT/*
    do
    FONT_SUFFIX=$(echo "`basename $FILE .h`" | grep -oP '(?<=pt8b)\w+')
    FONT_SIZE=$(echo "`basename $FILE .h`" | grep -oP '\d+(?=pt8b)')

    echo "#define FONT_"$FONT_SIZE"pt8b"$FONT_SUFFIX" `basename $FILE .h`" >> $HEADER_FILE
  done
  echo "#endif" >> $HEADER_FILE

done

