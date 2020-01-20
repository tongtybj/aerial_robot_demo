#!/usr/bin/env python

""" search fire in tower """

import rospy
import smach
import smach_ros

from hydrus_commander import HydrusCommander

from plane_search_states import *
from hydrus_navigation_states import *

from std_msgs.msg import Empty

def monitor_cb(ud, msg):
    return False

def main():
    """ main execution """
    rospy.init_node("find_ground_fire_motion")

    sm = smach.StateMachine(outcomes=['DONE', 'FAIL'])
    with sm:
        search_params = {}
        search_params['target_topic_name'] = rospy.get_param('~target_topic_name', '/target_object/pos')
        search_params['control_rate'] = rospy.get_param('~control_rate', 5.0)
        search_params['search_grid_size'] = rospy.get_param('~search_grid_size', 1.0)
        search_params['reach_margin'] = rospy.get_param('~reach_margin', 0.2)
        search_params['search_height'] = rospy.get_param('~search_height', 3.0)
        search_params['target_timeout'] = rospy.get_param('~target_timeout', 1.0)
        search_params['approach_height'] = rospy.get_param('~approach_height', 3.0)
        search_params['descending_height'] = rospy.get_param('~descending_height', 2.0)
        search_params['approach_margin'] = rospy.get_param('~approach_margin', 0.05)
        search_params['height_margin'] = rospy.get_param('~height_margin', 0.05)
        search_params['covering_pre_height'] = rospy.get_param('~covering_pre_height', 1.0)
        search_params['covering_post_height'] = rospy.get_param('~covering_post_height', 0.2)
        search_params['covering_move_dist'] = rospy.get_param('~covering_move_dist', 1.0)

        smach.StateMachine.add('INITIAL_STATE', smach_ros.MonitorState("~task3_start", Empty, monitor_cb), transitions={'invalid':'TAKEOFF', 'valid':'INITIAL_STATE', 'preempted':'INITIAL_STATE'})
        smach.StateMachine.add('TAKEOFF', TakeoffState(), transitions={'success':'NAVIGATING0', 'fail':'INITIAL_STATE'})

        search_area_number = rospy.get_param('~search_area_number', 1)
        waypoints_list = rospy.get_param('~initial_waypoints', [[[0,0,1]]])
        area_corners_list = rospy.get_param('~area_corners', [[[0,0],[2,2]]])
        for i in range(search_area_number):
            waypoint_nav_creator = WaypointNavigationStateMachineCreator()
            waypoints = waypoints_list[i]
            sm_sub_nav = waypoint_nav_creator.create(waypoints)
            smach.StateMachine.add('NAVIGATING'+str(i), sm_sub_nav, transitions={'success':'SEARCHING'+str(i), 'failure':'FAIL'})

            search_params['area_corners'] = area_corners_list[i]
            search_sm_creator = PlaneFireFightStateMachineCreator()
            sm_sub = search_sm_creator.create('rectangular_grid', search_params)
            if i==search_area_number-1:
                next_state = 'FAIL'
            else:
                next_state = 'NAVIGATING'+str(i+1)
            smach.StateMachine.add('SEARCHING'+str(i), sm_sub, transitions={'success':'DONE', 'failure':next_state})

    sis = smach_ros.IntrospectionServer('smach_server', sm, '/SM_ROOT')
    sis.start()
    sm.execute()
    rospy.spin()
    sis.stop()

if __name__ == "__main__":
    main()

